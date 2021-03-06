/// \file conn_http.cpp
/// Contains the main code for the HTTP Connector

#include <iostream>
#include <queue>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <getopt.h>
#include <set>
#include <mist/socket.h>
#include <mist/http_parser.h>
#include <mist/config.h>
#include <mist/stream.h>
#include <mist/timing.h>
#include <mist/auth.h>
#include "tinythread.h"
#include "embed.js.h"

/// Holds everything unique to HTTP Connector.
namespace Connector_HTTP {

  /// Class for keeping track of connections to connectors.
  class ConnConn{
    public:
      Socket::Connection * conn; ///< The socket of this connection
      unsigned int lastuse; ///< Seconds since last use of this connection.
      tthread::mutex in_use; ///< Mutex for this connection.
      /// Constructor that sets the socket and lastuse to 0.
      ConnConn(){
        conn = 0;
        lastuse = 0;
      }
      ;
      /// Constructor that sets lastuse to 0, but socket to s.
      ConnConn(Socket::Connection * s){
        conn = s;
        lastuse = 0;
      }
      ;
      /// Destructor that deletes the socket if non-null.
      ~ConnConn(){
        if (conn){
          conn->close();
          delete conn;
        }
        conn = 0;
      }
      ;
  };

  std::map<std::string, ConnConn *> connconn; ///< Connections to connectors
  std::set<tthread::thread *> active_threads; ///< Holds currently active threads
  std::set<tthread::thread *> done_threads; ///< Holds threads that are done and ready to be joined.
  tthread::mutex thread_mutex; ///< Mutex for adding/removing threads.
  tthread::mutex conn_mutex; ///< Mutex for adding/removing connector connections.
  tthread::mutex timeout_mutex; ///< Mutex for timeout thread.
  tthread::thread * timeouter = 0; ///< Thread that times out connections to connectors.

  void Timeout_Thread(void * n){
    n = 0; //prevent unused variable warning
    tthread::lock_guard<tthread::mutex> guard(timeout_mutex);
    while (true){
      {
        tthread::lock_guard<tthread::mutex> guard(conn_mutex);
        if (connconn.empty()){
          return;
        }
        std::map<std::string, ConnConn*>::iterator it;
        for (it = connconn.begin(); it != connconn.end(); it++){
          if ( !it->second->conn->connected() || it->second->lastuse++ > 15){
            if (it->second->in_use.try_lock()){
              it->second->in_use.unlock();
              delete it->second;
              connconn.erase(it);
              it = connconn.begin(); //get a valid iterator
              if (it == connconn.end()){
                return;
              }
            }
          }
        }
        conn_mutex.unlock();
      }
      usleep(1000000); //sleep 1 second and re-check
    }
  }

  /// Handles requests without associated handler, displaying a nice friendly error message.
  void Handle_None(HTTP::Parser & H, Socket::Connection * conn){
    H.Clean();
    H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
    H.SetBody(
        "<!DOCTYPE html><html><head><title>Unsupported Media Type</title></head><body><h1>Unsupported Media Type</h1>The server isn't quite sure what you wanted to receive from it.</body></html>");
    conn->SendNow(H.BuildResponse("415", "Unsupported Media Type"));
  }

  void Handle_Timeout(HTTP::Parser & H, Socket::Connection * conn){
    H.Clean();
    H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
    H.SetBody(
        "<!DOCTYPE html><html><head><title>Gateway timeout</title></head><body><h1>Gateway timeout</h1>Though the server understood your request and attempted to handle it, somehow handling it took longer than it should. Your request has been cancelled - please try again later.</body></html>");
    conn->SendNow(H.BuildResponse("504", "Gateway Timeout"));
  }

  /// Handles internal requests.
  void Handle_Internal(HTTP::Parser & H, Socket::Connection * conn){

    std::string url = H.getUrl();

    if (url == "/crossdomain.xml"){
      H.Clean();
      H.SetHeader("Content-Type", "text/xml");
      H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
      H.SetBody(
          "<?xml version=\"1.0\"?><!DOCTYPE cross-domain-policy SYSTEM \"http://www.adobe.com/xml/dtds/cross-domain-policy.dtd\"><cross-domain-policy><allow-access-from domain=\"*\" /><site-control permitted-cross-domain-policies=\"all\"/></cross-domain-policy>");
      conn->SendNow(H.BuildResponse("200", "OK"));
      return;
    } //crossdomain.xml

    if (url == "/clientaccesspolicy.xml"){
      H.Clean();
      H.SetHeader("Content-Type", "text/xml");
      H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
      H.SetBody(
          "<?xml version=\"1.0\" encoding=\"utf-8\"?><access-policy><cross-domain-access><policy><allow-from http-methods=\"*\" http-request-headers=\"*\"><domain uri=\"*\"/></allow-from><grant-to><resource path=\"/\" include-subpaths=\"true\"/></grant-to></policy></cross-domain-access></access-policy>");
      conn->SendNow(H.BuildResponse("200", "OK"));
      return;
    } //clientaccesspolicy.xml

    if ((url.length() > 9 && url.substr(0, 6) == "/info_" && url.substr(url.length() - 3, 3) == ".js")
        || (url.length() > 10 && url.substr(0, 7) == "/embed_" && url.substr(url.length() - 3, 3) == ".js")){
      std::string streamname;
      if (url.substr(0, 6) == "/info_"){
        streamname = url.substr(6, url.length() - 9);
      }else{
        streamname = url.substr(7, url.length() - 10);
      }
      Util::Stream::sanitizeName(streamname);
      JSON::Value ServConf = JSON::fromFile("/tmp/mist/streamlist");
      std::string response;
      std::string host = H.GetHeader("Host");
      if (host.find(':')){
        host.resize(host.find(':'));
      }
      H.Clean();
      H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
      H.SetHeader("Content-Type", "application/javascript");
      response = "// Generating info code for stream " + streamname + "\n\nif (!mistvideo){var mistvideo = {};}\n";
      JSON::Value json_resp;
      if (ServConf["streams"].isMember(streamname) && ServConf["config"]["protocols"].size() > 0){
        json_resp["width"] = ServConf["streams"][streamname]["meta"]["video"]["width"].asInt();
        json_resp["height"] = ServConf["streams"][streamname]["meta"]["video"]["height"].asInt();
        //first, see if we have RTMP working and output all the RTMP.
        for (JSON::ArrIter it = ServConf["config"]["protocols"].ArrBegin(); it != ServConf["config"]["protocols"].ArrEnd(); it++){
          if (( *it)["connector"].asString() == "RTMP"){
            JSON::Value tmp;
            tmp["type"] = "rtmp";
            tmp["url"] = "rtmp://" + host + ":" + ( *it)["port"].asString() + "/play/" + streamname;
            json_resp["source"].append(tmp);
          }
        }
        //then, see if we have HTTP working and output all the dynamic.
        for (JSON::ArrIter it = ServConf["config"]["protocols"].ArrBegin(); it != ServConf["config"]["protocols"].ArrEnd(); it++){
          if (( *it)["connector"].asString() == "HTTP"){
            JSON::Value tmp;
            tmp["type"] = "f4v";
            tmp["url"] = "http://" + host + ":" + ( *it)["port"].asString() + "/" + streamname + "/manifest.f4m";
            json_resp["source"].append(tmp);
          }
        }
        //and all the progressive.
        for (JSON::ArrIter it = ServConf["config"]["protocols"].ArrBegin(); it != ServConf["config"]["protocols"].ArrEnd(); it++){
          if (( *it)["connector"].asString() == "HTTP"){
            JSON::Value tmp;
            tmp["type"] = "flv";
            tmp["url"] = "http://" + host + ":" + ( *it)["port"].asString() + "/" + streamname + ".flv";
            json_resp["source"].append(tmp);
          }
        }
      }else{
        json_resp["error"] = "The specified stream is not available on this server.";
        json_resp["bbq"] = "sauce"; //for legacy purposes ^_^
      }
      response += "mistvideo['" + streamname + "'] = " + json_resp.toString() + ";\n";
      if (url.substr(0, 6) != "/info_" && !json_resp.isMember("error")){
        response.append("\n(");
        response.append((char*)embed_js, (size_t)embed_js_len - 2); //remove trailing ";\n" from xxd conversion
        response.append("(\"" + streamname + "\"));\n");
      }
      H.SetBody(response);
      conn->SendNow(H.BuildResponse("200", "OK"));
      return;
    } //embed code generator

    Handle_None(H, conn); //anything else doesn't get handled
  }

  /// Handles requests without associated handler, displaying a nice friendly error message.
  void Handle_Through_Connector(HTTP::Parser & H, Socket::Connection * conn, std::string & connector){
    //create a unique ID based on a hash of the user agent and host, followed by the stream name and connector
    std::string uid = Secure::md5(H.GetHeader("User-Agent") + conn->getHost()) + "_" + H.GetVar("stream") + "_" + connector;
    H.SetHeader("X-UID", uid); //add the UID to the headers before copying
    H.SetHeader("X-Origin", conn->getHost()); //add the UID to the headers before copying
    std::string request = H.BuildRequest(); //copy the request for later forwarding to the connector
    std::string orig_url = H.getUrl();
    H.Clean();

    //check if a connection exists, and if not create one
    conn_mutex.lock();
    if ( !connconn.count(uid) || !connconn[uid]->conn->connected()){
      if (connconn.count(uid)){
        connconn.erase(uid);
      }
      connconn[uid] = new ConnConn(new Socket::Connection("/tmp/mist/http_" + connector));
      connconn[uid]->conn->setBlocking(false); //do not block on spool() with no data
#if DEBUG >= 4
      std::cout << "Created new connection " << uid << std::endl;
#endif
    }else{
#if DEBUG >= 4
      std::cout << "Re-using connection " << uid << std::endl;
#endif
    }
    //start a new timeout thread, if neccesary
    if (timeout_mutex.try_lock()){
      if (timeouter){
        timeouter->join();
        delete timeouter;
      }
      timeouter = new tthread::thread(Connector_HTTP::Timeout_Thread, 0);
      timeout_mutex.unlock();
    }
    conn_mutex.unlock();

    //lock the mutex for this connection, and handle the request
    tthread::lock_guard<tthread::mutex> guard(connconn[uid]->in_use);
    //if the server connection is dead, handle as timeout.
    if ( !connconn.count(uid) || !connconn[uid]->conn->connected()){
      Handle_Timeout(H, conn);
      return;
    }
    //forward the original request
    connconn[uid]->conn->SendNow(request);
    connconn[uid]->lastuse = 0;
    unsigned int timeout = 0;
    //wait for a response
    while (connconn.count(uid) && connconn[uid]->conn->connected() && conn->connected()){
      conn->spool();
      if (connconn[uid]->conn->Received().size() || connconn[uid]->conn->spool()){
        //make sure we end in a \n
        if ( *(connconn[uid]->conn->Received().get().rbegin()) != '\n'){
          std::string tmp = connconn[uid]->conn->Received().get();
          connconn[uid]->conn->Received().get().clear();
          if (connconn[uid]->conn->Received().size()){
            connconn[uid]->conn->Received().get().insert(0, tmp);
          }else{
            connconn[uid]->conn->Received().append(tmp);
          }
        }
        //check if the whole response was received
        if (H.Read(connconn[uid]->conn->Received().get())){
          break; //continue down below this while loop
        }
      }else{
        //keep trying unless the timeout triggers
        if (timeout++ > 4000){
          std::cout << "[20s timeout triggered]" << std::endl;
          Handle_Timeout(H, conn);
          return;
        }else{
          Util::sleep(5);
        }
      }
    }
    if ( !connconn.count(uid) || !connconn[uid]->conn->connected() || !conn->connected()){
      //failure, disconnect and sent error to user
      Handle_Timeout(H, conn);
      return;
    }else{
      //success, check type of response
      if (H.GetHeader("Content-Length") != ""){
        //known length - simply re-send the request with added headers and continue
        H.SetHeader("X-UID", uid);
        H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
        conn->SendNow(H.BuildResponse("200", "OK"));
        conn->flush();
      }else{
        //unknown length
        H.SetHeader("X-UID", uid);
        H.SetHeader("Server", "mistserver/" PACKAGE_VERSION "/" + Util::Config::libver);
        conn->SendNow(H.BuildResponse("200", "OK"));
        //switch out the connection for an empty one - it makes no sense to keep these globally
        Socket::Connection * myConn = connconn[uid]->conn;
        connconn[uid]->conn = new Socket::Connection();
        connconn[uid]->in_use.unlock();
        //continue sending data from this socket and keep it permanently in use
        while (myConn->connected() && conn->connected()){
          if (myConn->Received().size() || myConn->spool()){
            //forward any and all incoming data directly without parsing
            conn->SendNow(myConn->Received().get());
            myConn->Received().get().clear();
          }else{
            Util::sleep(30);
          }
        }
        myConn->close();
        delete myConn;
        conn->close();
      }
    }
  }

  /// Returns the name of the HTTP connector the given request should be served by.
  /// Can currently return:
  /// - none (request not supported)
  /// - internal (request fed from information internal to this connector)
  /// - dynamic (request fed from http_dynamic connector)
  /// - progressive (request fed from http_progressive connector)
  std::string getHTTPType(HTTP::Parser & H){
    std::string url = H.getUrl();
    if ((url.find("f4m") != std::string::npos) || ((url.find("Seg") != std::string::npos) && (url.find("Frag") != std::string::npos))){
      std::string streamname = url.substr(1, url.find("/", 1) - 1);
      Util::Stream::sanitizeName(streamname);
      H.SetVar("stream", streamname);
      return "dynamic";
    }
    if (url.find("/smooth/") != std::string::npos && url.find(".ism") != std::string::npos){
      std::string streamname = url.substr(8, url.find("/", 8) - 12);
      Util::Stream::sanitizeName(streamname);
      H.SetVar("stream", streamname);
      return "smooth";
    }
    if (url.find("/hls/") != std::string::npos && (url.find(".m3u") != std::string::npos || url.find(".ts") != std::string::npos)){
      std::string streamname = url.substr(5, url.find("/", 5) - 5);
      Util::Stream::sanitizeName(streamname);
      H.SetVar("stream", streamname);
      return "live";
    }
    if (url.length() > 4){
      std::string ext = url.substr(url.length() - 4, 4);
      if (ext == ".flv" || ext == ".mp3"){
        std::string streamname = url.substr(1, url.length() - 5);
        Util::Stream::sanitizeName(streamname);
        H.SetVar("stream", streamname);
        return "progressive";
      }
    }
    if (url == "/crossdomain.xml"){
      return "internal";
    }
    if (url == "/clientaccesspolicy.xml"){
      return "internal";
    }
    if (url.length() > 10 && url.substr(0, 7) == "/embed_" && url.substr(url.length() - 3, 3) == ".js"){
      return "internal";
    }
    if (url.length() > 9 && url.substr(0, 6) == "/info_" && url.substr(url.length() - 3, 3) == ".js"){
      return "internal";
    }
    return "none";
  }

  /// Thread for handling a single HTTP connection
  void Handle_HTTP_Connection(void * pointer){
    Socket::Connection * conn = (Socket::Connection *)pointer;
    conn->setBlocking(false); //do not block on conn.spool() when no data is available
    HTTP::Parser Client;
    while (conn->connected()){
      if (conn->spool() || conn->Received().size()){
        //make sure it ends in a \n
        if ( *(conn->Received().get().rbegin()) != '\n'){
          std::string tmp = conn->Received().get();
          conn->Received().get().clear();
          if (conn->Received().size()){
            conn->Received().get().insert(0, tmp);
          }else{
            conn->Received().append(tmp);
          }
        }
        if (Client.Read(conn->Received().get())){
          std::string handler = getHTTPType(Client);
          long long int startms = Util::getMS();
#if DEBUG >= 4
          std::cout << "Received request: " << Client.getUrl() << " (" << conn->getSocket() << ") => " << handler << " (" << Client.GetVar("stream")
              << ")" << std::endl;
#endif
          bool closeConnection = false;
          if (Client.GetHeader("Connection") == "close"){
            closeConnection = true;
          }

          if (handler == "none" || handler == "internal"){
            if (handler == "internal"){
              Handle_Internal(Client, conn);
            }else{
              Handle_None(Client, conn);
            }
          }else{
            Handle_Through_Connector(Client, conn, handler);
          }
#if DEBUG >= 4
          std::cout << "Completed request (" << conn->getSocket() << ") " << handler << " in " << (Util::getMS() - startms) << " ms" << std::endl;
#endif
          if (closeConnection){
            break;
          }
          Client.Clean(); //clean for any possible next requests
        }
      }else{
        Util::sleep(10); //sleep 10ms
      }
    }
    //close and remove the connection
    conn->close();
    delete conn;
    //remove this thread from active_threads and add it to done_threads.
    thread_mutex.lock();
    for (std::set<tthread::thread *>::iterator it = active_threads.begin(); it != active_threads.end(); it++){
      if (( *it)->get_id() == tthread::this_thread::get_id()){
        tthread::thread * T = ( *it);
        active_threads.erase(T);
        done_threads.insert(T);
        break;
      }
    }
    thread_mutex.unlock();
  }

} //Connector_HTTP namespace

int main(int argc, char ** argv){
  Util::Config conf(argv[0], PACKAGE_VERSION);
  conf.addConnectorOptions(8080);
  conf.parseArgs(argc, argv);
  Socket::Server server_socket = Socket::Server(conf.getInteger("listen_port"), conf.getString("listen_interface"));
  if ( !server_socket.connected()){
    return 1;
  }
  conf.activate();

  while (server_socket.connected() && conf.is_active){
    Socket::Connection S = server_socket.accept();
    if (S.connected()){ //check if the new connection is valid
      //lock the thread mutex and spawn a new thread for this connection
      Connector_HTTP::thread_mutex.lock();
      tthread::thread * T = new tthread::thread(Connector_HTTP::Handle_HTTP_Connection, (void *)(new Socket::Connection(S)));
      Connector_HTTP::active_threads.insert(T);
      //clean up any threads that may have finished
      while ( !Connector_HTTP::done_threads.empty()){
        T = *Connector_HTTP::done_threads.begin();
        T->join();
        Connector_HTTP::done_threads.erase(T);
        delete T;
      }
      Connector_HTTP::thread_mutex.unlock();
    }else{
      Util::sleep(10); //sleep 10ms
    }
  } //while connected and not requested to stop
  server_socket.close();

  //wait for existing connections to drop
  bool repeat = true;
  while (repeat){
    Connector_HTTP::thread_mutex.lock();
    repeat = !Connector_HTTP::active_threads.empty();
    //clean up any threads that may have finished
    while ( !Connector_HTTP::done_threads.empty()){
      tthread::thread * T = *Connector_HTTP::done_threads.begin();
      T->join();
      Connector_HTTP::done_threads.erase(T);
      delete T;
    }
    Connector_HTTP::thread_mutex.unlock();
    if (repeat){
      Util::sleep(100); //sleep 100ms
    }
  }

  return 0;
} //main
