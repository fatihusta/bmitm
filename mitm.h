
#pragma once

#include <commoncpp/tcp.h>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include <functional>
#include "log.h"

namespace ph = std::placeholders;

typedef std::function<bool(uint, std::string &d)>data_cb_t;
typedef std::function<void(uint, uint)>conn_cb_t;

#define MAX_MESSAGE_SIZE 	8192
#define PENDING_SLEEP_MS	10

enum {
  APP_CONNECTED,
  APP_DISCONNECTED,
  APP_ERROR,
  NET_ERROR,
  OTHER_ERROR
};

class mitm_conn_t {
  uint cid, debug;
  ost::TCPStream *app, *net;
  char buf[MAX_MESSAGE_SIZE];
  data_cb_t app_tx, app_rx;
  conn_cb_t app_conn;
  inline bool isDelim(char c, const std::string &delims) {
    for (uint i = 0; i < delims.size(); ++i) {
      if (delims[i] == c)
        return true;
    }
    return false;
  }
  std::vector<std::string> split(const std::string& str, const std::string& delims) {
    std::vector<std::string> wordVector;
    std::stringstream stringStream(str);
    std::string word; char c = 0;
    while (stringStream) {
      word.clear();
      while (stringStream && !isDelim((c = stringStream.get()), delims) && (c != EOF))
        word.push_back(c);
      if (c != EOF)
        stringStream.unget();
      wordVector.push_back(word);
      while (stringStream && isDelim((c = stringStream.get()), delims));
      if (c != EOF)
       stringStream.unget();
   }
   return wordVector;
  }
  bool process_app_message(char *b, ssize_t sz) {
    // SSLproxy: [127.0.0.1]:44145,[10.1.1.243]:55416,[172.217.16.14]:443,s\r\n
    //             proxy reply ep         source              dst
    char *pos = (char *)memmem(b, sz, "\r\n", 2);
    const char *prefix = "SSLproxy: [";
    if (!net && !strncmp(prefix, b, strlen(prefix)) && pos) {
      ssize_t margin = pos - b + 2;
      std::string h(b, margin - 2);
      std::string q(b + margin, sz - margin);
      Log(LOG_DEBUG_LVL) << "conn N" << cid << " app: h: " << h;
      if (debug)
        Log(LOG_DEBUG_LVL) << "conn N" << cid << " app: sz: " << q.size() << ", q: " << q;
      std::vector<std::string> ha = split(h, "[]:, \r\n");
      net = new ost::TCPStream(ost::InetHostAddress(ha[1].c_str()), atoi(ha[2].c_str()));
      if (!app_tx(cid, q) || !net->isConnected() ||
        (net->writeData(q.data(), q.size(), 0) != (ssize_t)q.size()))
          return false;
    } else if (net) {
        std::string q(b, sz);
        if (debug)
          Log(LOG_DEBUG_LVL) << "conn N" << cid << " app: sz: " << q.size();
        if (!app_tx(cid, q) || !net->isConnected() ||
          (net->writeData(q.data(), q.size(), 0) != (ssize_t)q.size()))
            return false;
    } else {
        std::string q(b, sz);
        Log(LOG_WARNING_LVL) << "conn N" << cid << " : mitm_conn_t::process_app_message: unhandled message [" <<
          q << "] !";
        app_conn(cid, APP_ERROR);
        return false;
    }
    return true;
  }
  bool process_network_message(char *b, ssize_t sz) {
    std::string r(buf, sz);
    if (debug)
      Log(LOG_DEBUG_LVL) << "conn N" << cid << " net: sz: " << sz << ", [" << r << "]";
    if (!app_rx(cid, r) || !app->isConnected() ||
      (app->writeData(r.data(), r.size()) != (ssize_t)r.size()))
        return false;
    return true;
  }
  // TODO: move app/net->isPending() branches to own separate threads, will improve perfomance
  bool mitm_run() {
    // app side
    if (app->isPending(ost::Socket::pendingInput, PENDING_SLEEP_MS)) {
      ssize_t sz = app->readData(buf, sizeof(buf));
      if (!sz || !process_app_message(buf, sz))
        return false;
    }
    if (app->isPending(ost::Socket::pendingError, PENDING_SLEEP_MS)) {
      Log(LOG_WARNING_LVL) << "conn N" << cid << " app: Pending error";
      app_conn(cid, APP_ERROR);
      return false;
    }
    // net side
    if (net && net->isPending(ost::Socket::pendingInput, PENDING_SLEEP_MS)) {
      ssize_t sz = net->readData(buf, sizeof(buf));
      if (!sz || !process_network_message(buf, sz))
        return false;
    }
    if (net && net->isPending(ost::Socket::pendingError, PENDING_SLEEP_MS)) {
      app_conn(cid, NET_ERROR);
      Log(LOG_WARNING_LVL) << "conn N" << cid << " net: Pending error";
      return false;
    }
    return true;
 }
 public:
  mitm_conn_t(uint cid_, ost::TCPStream *app_, data_cb_t app_tx_, data_cb_t app_rx_, conn_cb_t app_conn_) :
    cid(cid_), debug(0), app(app_), net(0), app_tx(app_tx_), app_rx(app_rx_), app_conn(app_conn_) { }
  ~mitm_conn_t() {
    if (net)
      delete net;
  }
  void run() {
    try {
      app_conn(cid, APP_CONNECTED);
      while(mitm_run())
        std::this_thread::sleep_for(std::chrono::milliseconds(PENDING_SLEEP_MS));
    } catch (ost::Socket* s) {
        int err = s->getErrorNumber() ? s->getErrorNumber() : s->getSystemError();
        app_conn(cid, OTHER_ERROR);
        Log(LOG_WARNING_LVL) << "mitm_run: conn N" << cid << " : Socket exception: " << strerror(err);
    } catch (...) {
        app_conn(cid, OTHER_ERROR);
        Log(LOG_WARNING_LVL) << "mitm_run: conn N" << cid << " : general exception";
    }
    if (net->isConnected())
      net->disconnect();
    if (app->isConnected())
      app->disconnect();
    app_conn(cid, APP_DISCONNECTED);
    Log(LOG_DEBUG_LVL) << "conn N" << cid << " disconnected";
  }
};

class autodeleted_thread {
  std::thread t;
 public:
  explicit autodeleted_thread(std::thread t_): t(std::move(t_)) {
    if (!t.joinable())
      throw std::logic_error("No thread !");
  }
  ~autodeleted_thread() { t.detach(); }
  autodeleted_thread(autodeleted_thread&)= delete;
  autodeleted_thread& operator=(autodeleted_thread const &)= delete;
};

class mitm_t : public ost::TCPSocket, public ost::Thread {
  data_cb_t app_tx, app_rx;
  conn_cb_t app_conn;
  bool app_tx_cb(uint cid, std::string &d) { return app_tx(cid, d); }
  bool app_rx_cb(uint cid, std::string &d) { return app_rx(cid, d); }
  void app_conn_cb(uint cid, uint ev) { app_conn(cid, ev); }
 public:
  static const char *ev2str(int ev) {
    switch (ev) {
      case APP_CONNECTED:
        return "app connected";
      case APP_DISCONNECTED:
        return "app disconnected";
      case APP_ERROR:
        return "app error";
      case NET_ERROR:
        return "net error";
      case OTHER_ERROR:
        return "other error";
      default:
        break;
    }
    return "unknown";
  }
  mitm_t(ost::tpport_t p, data_cb_t app_tx_, data_cb_t app_rx_, conn_cb_t app_conn_) :
    TCPSocket(ost::IPV4Address("0.0.0.0"), p), app_tx(app_tx_), app_rx(app_rx_), app_conn(app_conn_) { }
  void run() {
    static uint cid;
    while(1) {
      if (isPendingConnection()) {
        auto process_mitm = [](const int cid_, mitm_t *m) {
          // TCPStream(TCPSocket &server, bool throwflag = true, timeout_t timeout = 0);
          ost::TCPStream *app = new ost::TCPStream(*m, true, PENDING_SLEEP_MS);
          mitm_conn_t mc(cid_, app, std::bind(&mitm_t::app_tx_cb, m, ph::_1, ph::_2),
            std::bind(&mitm_t::app_rx_cb, m, ph::_1, ph::_2),
            std::bind(&mitm_t::app_conn_cb, m, ph::_1, ph::_2));
          mc.run();
          delete app;
        };
        autodeleted_thread sthr(std::thread(process_mitm, cid, this));
        Log(LOG_DEBUG_LVL) << "mitm: new connection N" << cid++;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(PENDING_SLEEP_MS));
    }
  }
};
