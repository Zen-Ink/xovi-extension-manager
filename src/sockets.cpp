#include "sockets.h"
#include "diagnostics_qt.h"
#include "../sdk/xovi-sockets.h"
#include <QCoreApplication>
#include <QThread>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFile>
#include <map>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>

namespace {
constexpr int maxFrame=64*1024, maxOutput=128*1024, maxEvents=128;
struct Connection {
    uint64_t id;
    QPointer<QLocalSocket> socket;
    QPointer<QTimer> partialTimer;
    QByteArray buffer;
};
struct Service {
    uint64_t id;
    QString owner,name,path;
    XemSocketCallback callback;
    void *userData;
    bool active=true;
    bool resyncPending=false;
    QPointer<QLocalServer> server;
    std::unique_ptr<QLockFile> lock;
    std::map<uint64_t,std::shared_ptr<Connection>> clients;
};
std::map<uint64_t,std::shared_ptr<Service>> services;
uint64_t nextService=1,nextConnection=1;
int queuedEvents=0,clientCount=0;
bool mainThread() {
    return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();
}
char *json(const QJsonObject &object) {
    auto data=QJsonDocument(object).toJson(QJsonDocument::Compact);
    auto *result=static_cast<char *>(std::malloc(size_t(data.size())+1));
    if(result) {std::memcpy(result,data.constData(),size_t(data.size()));result[data.size()]='\0';}
    return result;
}
char *error(const char *code) {return json(withDiagnostic({{"ok",false},{"error",code}}));}
bool validId(const QString &id) {
    return QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,31}$").match(id).hasMatch();
}
bool parse(const char *text,QJsonObject &object) {
    if(!text || strnlen(text,maxFrame+1)>maxFrame) return false;
    QJsonParseError failure;
    auto document=QJsonDocument::fromJson(text,&failure);
    if(failure.error!=QJsonParseError::NoError || !document.isObject()) return false;
    object=document.object();return true;
}
QJsonObject description(const std::shared_ptr<Service> &s) {
    QJsonArray clients;
    for(const auto &pair:s->clients) clients.append(static_cast<qint64>(pair.first));
    return {{"serviceHandle",static_cast<qint64>(s->id)},{"ownerId",s->owner},{"serviceId",s->name},
        {"path",s->path},{"state",s->server && s->server->isListening() ? "listening" : "closed"},
        {"connections",clients},{"protocol","json-lines-v1"}};
}
bool event(const std::shared_ptr<Service> &service,QJsonObject message) {
    if(queuedEvents>=maxEvents) {
        // A bounded queue may lose lifecycle events under load. Always deliver
        // one coalesced resync hint so plugins can reconcile query(handle).
        if(!service->resyncPending) {
            service->resyncPending=true;
            QTimer::singleShot(0,QCoreApplication::instance(),[service] {
                service->resyncPending=false;
                const auto data=QJsonDocument(QJsonObject{{"type","resync"},
                    {"serviceHandle",static_cast<qint64>(service->id)}}).toJson(QJsonDocument::Compact);
                if(service->active) service->callback(data.constData(),service->userData);
            });
        }
        return false;
    }
    ++queuedEvents;
    message.insert("serviceHandle",static_cast<qint64>(service->id));
    const auto data=QJsonDocument(message).toJson(QJsonDocument::Compact);
    QTimer::singleShot(0,QCoreApplication::instance(),[service,data] {
        --queuedEvents;
        if(service->active) service->callback(data.constData(),service->userData);
    });
    return true;
}
void read(const std::shared_ptr<Service> &s,const std::shared_ptr<Connection> &c) {
    if(!s->active || !c->socket || !s->clients.count(c->id)) return;
    // Cap both stream buffers and queued callbacks; never wait for a client.
    c->buffer+=c->socket->read(maxFrame+1-c->buffer.size());
    for(int batch=0;batch<32;++batch) {
        const auto newline=c->buffer.indexOf('\n');
        if(newline<0) break;
        auto frame=c->buffer.left(newline);c->buffer.remove(0,newline+1);
        if(c->partialTimer) c->partialTimer->stop();
        QJsonParseError failure;
        auto document=QJsonDocument::fromJson(frame,&failure);
        if(frame.size()>maxFrame || failure.error!=QJsonParseError::NoError || !document.isObject()
            || !event(s,{{"type","message"},{"connectionId",static_cast<qint64>(c->id)},
                         {"message",document.object()}})) {c->socket->abort();return;}
    }
    if(c->buffer.size()>maxFrame) {c->socket->abort();return;}
    if(!c->buffer.isEmpty() && c->partialTimer && !c->partialTimer->isActive()) c->partialTimer->start();
    if(c->buffer.contains('\n') || c->socket->bytesAvailable()>0)
        QTimer::singleShot(0,QCoreApplication::instance(),[s,c]{read(s,c);});
}
char *registerService(const char *text,XemSocketCallback callback,void *userData) {
    if(!mainThread()) return error("application-thread-required");
    if(QFileInfo(QCoreApplication::applicationFilePath()).fileName()!="xochitl") return error("wrong-process");
    QJsonObject request;
    if(!callback || !parse(text,request)) return error("invalid-request");
    QString owner=request.value("ownerId").toString(), name=request.value("serviceId").toString();
    if(!validId(owner) || !validId(name)) return error("invalid-service-id");
    if(services.size()>=32) return error("service-limit");
    for(const auto &pair:services)
        if(pair.second->owner==owner && pair.second->name==name) return error("service-already-registered");
    const QString directory=QString("/tmp/xovi-em-%1").arg(static_cast<qulonglong>(geteuid()));
    const auto directoryBytes=QFile::encodeName(directory);
    if(::mkdir(directoryBytes.constData(),0700)!=0 && errno!=EEXIST) return error("runtime-directory-unavailable");
    struct stat statbuf{};
    if(::lstat(directoryBytes.constData(),&statbuf)!=0 || !S_ISDIR(statbuf.st_mode)
        || statbuf.st_uid!=geteuid() || (statbuf.st_mode&0077)) return error("unsafe-runtime-directory");
    auto s=std::make_shared<Service>();s->id=nextService++;s->owner=owner;s->name=name;
    s->path=directory+"/"+owner+"."+name+".sock";s->callback=callback;s->userData=userData;
    const auto path=QFile::encodeName(s->path);
    if(path.size()>=int(sizeof(sockaddr_un{}.sun_path))) return error("socket-path-too-long");
    s->lock=std::make_unique<QLockFile>(s->path+".lock");s->lock->setStaleLockTime(0);
    if(!s->lock->tryLock(0)) return error("socket-in-use");
    if(::lstat(path.constData(),&statbuf)==0) {
        if(!S_ISSOCK(statbuf.st_mode) || statbuf.st_uid!=geteuid()) return error("unsafe-socket-path");
        const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd<0) return error("socket-unavailable");
        sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path.constData(),size_t(path.size())+1);
        const int connected=::connect(fd,reinterpret_cast<sockaddr *>(&address),sizeof(address));
        const int reason=errno;::close(fd);
        if(connected==0 || (reason!=ECONNREFUSED && reason!=ENOENT)) return error("socket-in-use");
        if(!QLocalServer::removeServer(s->path)) return error("stale-socket-cleanup-failed");
    } else if(errno!=ENOENT) return error("socket-path-unavailable");
    s->server=new QLocalServer(QCoreApplication::instance());
    s->server->setSocketOptions(QLocalServer::UserAccessOption);
    s->server->setMaxPendingConnections(16);
    if(!s->server->listen(s->path)) {delete s->server.data();return error("listen-failed");}
    services.emplace(s->id,s);
    std::weak_ptr<Service> weak=s;
    QObject::connect(s->server.data(),&QLocalServer::newConnection,s->server.data(),[weak] {
        auto s=weak.lock();if(!s || !s->active || !s->server) return;
        while(s->server->hasPendingConnections()) {
            auto *socket=s->server->nextPendingConnection();
            if(!socket) break;
            if(s->clients.size()>=16 || clientCount>=64) {socket->abort();socket->deleteLater();continue;}
            auto c=std::make_shared<Connection>();c->id=nextConnection++;c->socket=socket;
            c->partialTimer=new QTimer(socket);c->partialTimer->setInterval(15000);c->partialTimer->setSingleShot(true);
            socket->setReadBufferSize(maxFrame+1);s->clients.emplace(c->id,c);++clientCount;
            QObject::connect(c->partialTimer.data(),&QTimer::timeout,socket,[socket]{socket->abort();});
            QObject::connect(socket,&QLocalSocket::readyRead,socket,[s,c]{read(s,c);});
            QObject::connect(socket,&QLocalSocket::disconnected,socket,[s,c] {
                if(s->clients.erase(c->id)) --clientCount;
                if(c->partialTimer) c->partialTimer->stop();
                if(s->active) event(s,{{"type","disconnected"},{"connectionId",static_cast<qint64>(c->id)}});
                if(c->socket) c->socket->deleteLater();
            });
            if(!event(s,{{"type","connected"},{"connectionId",static_cast<qint64>(c->id)}})) {socket->abort();continue;}
            if(socket->bytesAvailable()) read(s,c);
        }
    });
    auto response=description(s);response.insert("ok",true);return json(response);
}
char *unregisterService(uint64_t handle) {
    if(!mainThread()) return error("application-thread-required");
    auto found=services.find(handle);if(found==services.end()) return error("service-not-found");
    auto s=found->second;s->active=false;services.erase(found);
    if(s->server) s->server->close();
    auto clients=s->clients;
    for(const auto &pair:clients) if(pair.second->socket) pair.second->socket->abort();
    clientCount-=int(s->clients.size());s->clients.clear();
    if(s->server) s->server->deleteLater();
    s->lock->unlock();
    return json({{"ok",true},{"state","closed"}});
}
char *query(uint64_t handle) {
    if(!mainThread()) return error("application-thread-required");
    if(handle) {
        auto found=services.find(handle);if(found==services.end()) return error("service-not-found");
        auto result=description(found->second);result.insert("ok",true);return json(result);
    }
    QJsonArray result;for(const auto &pair:services) result.append(description(pair.second));
    return json({{"ok",true},{"services",result}});
}
char *send(uint64_t service,uint64_t connection,const char *text) {
    if(!mainThread()) return error("application-thread-required");
    auto found=services.find(service);if(found==services.end()) return error("service-not-found");
    auto client=found->second->clients.find(connection);
    if(client==found->second->clients.end() || !client->second->socket) return error("connection-not-found");
    auto *socket=client->second->socket.data();QJsonObject object;
    if(!parse(text,object)) return error("invalid-message");
    const auto frame=QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n';
    if(socket->bytesToWrite()+frame.size()>maxOutput) {socket->abort();return error("slow-client");}
    if(socket->write(frame)!=frame.size()) {socket->abort();return error("socket-write-failed");}
    return json({{"ok",true},{"status","queued"}});
}
char *disconnectClient(uint64_t service,uint64_t connection) {
    if(!mainThread()) return error("application-thread-required");
    auto found=services.find(service);if(found==services.end()) return error("service-not-found");
    auto client=found->second->clients.find(connection);
    if(client==found->second->clients.end()) return error("connection-not-found");
    if(client->second->socket) client->second->socket->abort();
    return json({{"ok",true}});
}
void freeString(char *text) {std::free(text);}
const XemSocketsApi api{XEM_SOCKETS_ABI,sizeof(XemSocketsApi),registerService,unregisterService,query,send,disconnectClient,freeString};
}
extern "C" const XemSocketsApi *xem_sockets_get_api(void) {return &api;}
