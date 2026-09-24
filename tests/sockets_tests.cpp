#include "../src/sockets.h"
#include "../sdk/xovi-sockets.h"
#include <QCoreApplication>
#include <QEvent>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLocalServer>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <functional>
#include <thread>
#include <cstdio>
#include <cstdlib>

static const XemSocketsApi *api;
static void check(bool ok,const char *message) {
    if(!ok) {std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}
    std::printf("PASS %s\n",message);
}
static QJsonObject reply(char *raw) {
    check(raw,"API returned JSON");
    auto result=QJsonDocument::fromJson(raw).object();api->freeString(raw);return result;
}
static bool until(const std::function<bool()> &condition) {
    QElapsedTimer timer;timer.start();
    while(!condition() && timer.elapsed()<1500) {QCoreApplication::processEvents();QThread::msleep(1);}
    return condition();
}
struct Consumer {uint64_t handle=0;int messages=0;bool closeOnMessage=false;};
static void onEvent(const char *text,void *context) {
    auto &consumer=*static_cast<Consumer *>(context);
    check(QThread::currentThread()==QCoreApplication::instance()->thread(),"callback application thread");
    const auto event=QJsonDocument::fromJson(text).object();
    if(event.value("type")!="message") return;
    ++consumer.messages;
    // Native API calls are safe inside callbacks.
    if(consumer.closeOnMessage) {reply(api->unregisterService(consumer.handle));return;}
    auto frame=QJsonDocument(event.value("message").toObject()).toJson(QJsonDocument::Compact);
    check(reply(api->send(consumer.handle,event.value("connectionId").toInteger(),frame.constData())).value("ok").toBool(),"echo accepted");
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);api=xem_sockets_get_api();
    Consumer consumer;
    const auto request=QJsonDocument(QJsonObject{{"ownerId",QString("socket-test-%1").arg(app.applicationPid())},{"serviceId","control"}}).toJson(QJsonDocument::Compact);
    auto registered=reply(api->registerService(request.constData(),onEvent,&consumer));
    if(QFileInfo(app.applicationFilePath()).fileName()!="xochitl") {
        check(registered.value("error")=="wrong-process","renderer cannot register endpoints");return 0;
    }
    check(registered.value("ok").toBool(),"register actual local socket");
    consumer.handle=registered.value("serviceHandle").toInteger();const auto path=registered.value("path").toString();
    check(reply(api->registerService(request.constData(),onEvent,&consumer)).value("error")=="service-already-registered","duplicate registration rejected");
    QJsonObject workerResult;
    std::thread worker([&]{workerResult=reply(api->query(consumer.handle));});worker.join();
    check(workerResult.value("error")=="application-thread-required","worker call rejected without touching socket objects");
    QLocalSocket client;client.connectToServer(path);
    check(until([&]{return client.state()==QLocalSocket::ConnectedState;}),"external client connected");
    client.write("{\"id\":1");client.flush();
    QCoreApplication::processEvents();check(consumer.messages==0,"partial frame is not dispatched");
    client.write("}\n{\"id\":2}\n");client.flush();
    check(until([&]{return consumer.messages==2 && client.bytesAvailable()>0;}),"split and batched frames dispatched");
    QByteArray response;
    check(until([&]{response+=client.readAll();return response.count('\n')==2;}),"two complete echo replies");
    check(reply(api->query(consumer.handle)).value("connections").toArray().size()==1,"state query reports live connection");
    client.write("not-json\n");client.flush();
    check(until([&]{return client.state()==QLocalSocket::UnconnectedState;}),"malformed peer is disconnected");
    QLocalSocket oversized;oversized.connectToServer(path);
    check(until([&]{return oversized.state()==QLocalSocket::ConnectedState;}),"second client connected");
    oversized.write(QByteArray(64*1024+1,'x'));oversized.flush();
    check(until([&]{return oversized.state()==QLocalSocket::UnconnectedState;}),"oversized incomplete frame is bounded");
    QLocalSocket closing;closing.connectToServer(path);
    check(until([&]{return closing.state()==QLocalSocket::ConnectedState;}),"third client connected");
    consumer.closeOnMessage=true;
    closing.write("{\"id\":3}\n{\"id\":4}\n");closing.flush();
    check(until([&]{return closing.state()==QLocalSocket::UnconnectedState;}),"unregister in callback closes clients");
    QCoreApplication::processEvents();
    check(consumer.messages==3,"unregister suppresses queued second callback");
    check(!QFile::exists(path),"unregister removes listening path");
    check(reply(api->query(consumer.handle)).value("error")=="service-not-found","closed handle cannot be queried");
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    // Never unlink another live listener, even if it has no manager lock file.
    QLocalServer foreign;check(foreign.listen(path),"foreign listener owns path");
    check(reply(api->registerService(request.constData(),onEvent,&consumer)).value("error")=="socket-in-use","live foreign socket preserved");
    check(foreign.isListening() && QFile::exists(path),"foreign socket remains usable");foreign.close();
    QFile conflict(path);check(conflict.open(QIODevice::WriteOnly),"conflicting file created");conflict.write("keep");conflict.close();
    check(reply(api->registerService(request.constData(),onEvent,&consumer)).value("error")=="unsafe-socket-path","regular file never replaced");
    check(conflict.open(QIODevice::ReadOnly) && conflict.readAll()=="keep","conflicting data preserved");conflict.close();conflict.remove();
    const int stale=::socket(AF_UNIX,SOCK_STREAM,0);
    check(stale>=0,"create stale listener fixture");
    sockaddr_un address{};address.sun_family=AF_UNIX;
    const auto encoded=QFile::encodeName(path);
    std::memcpy(address.sun_path,encoded.constData(),size_t(encoded.size())+1);
    check(::bind(stale,reinterpret_cast<sockaddr *>(&address),sizeof(address))==0,"bind abandoned socket");
    ::close(stale); // A crashed process leaves the filesystem socket behind.
    registered=reply(api->registerService(request.constData(),onEvent,&consumer));
    check(registered.value("ok").toBool(),"service removes only the verified stale socket and registers again");
    reply(api->unregisterService(registered.value("serviceHandle").toInteger()));
    return 0;
}
