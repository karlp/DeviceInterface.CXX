#include <labnation/interfaceserver.h>
#ifndef DNSSD
#include <avahi-common/error.h>
#endif
#include <labnation.h>
#include <errno.h>
#include <string.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <utils.h>
#include <netinet/tcp.h>

namespace labnation {

NetException::NetException(const char* message, ...) {
  char msg[255];
  va_list vl;
  va_start(vl, message);
  std::vsprintf(msg, message, vl);
  va_end(vl);
  _message = std::string(msg);
}

InterfaceServer::InterfaceServer(SmartScopeUsb* scope) {
  debug("====================NEW SERVER====================");
#ifndef DNSSD
  _avahi_poll = avahi_threaded_poll_new();
  _avahi_client = avahi_client_new(avahi_threaded_poll_get(_avahi_poll), AVAHI_CLIENT_NO_FAIL,
                                   InterfaceServer::AvahiCallback, this, NULL);
#endif

  _scope = scope;
  _state = Uninitialized;
  _stateRequested = Stopped;
  pthread_create(&_thread_state, NULL, ThreadStartManageState, this);
}

InterfaceServer::~InterfaceServer() {
#ifndef DNSSD
  avahi_threaded_poll_stop(_avahi_poll);
  avahi_client_free(_avahi_client);
  avahi_threaded_poll_free(_avahi_poll);
#endif
  debug("destructing interface server");
  while(!(GetState() == Destroyed))
    Destroy();
  pthread_join(_thread_state, NULL);
}

void* InterfaceServer::ThreadStartManageState(void * ctx) {
  PTHREAD_NAME("smartscope.server.state-manager")
  ((InterfaceServer*)ctx)->ManageState();
  return NULL;
}

void InterfaceServer::ManageState() {
  while (_state != Destroyed)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (_state == Destroying || _state == Starting || _state == Stopping)
      throw new NetException("Server state transitioning outside of state manager thread");

    //Local copy of stateRequested so code below is thread safe
    State nextState = _stateRequested;
    if (nextState == _state)
      continue;

    switch (nextState)
    {
    //From here, stateRequested can only be Started, Stopped or Destroyed
    case Started:
      debug("=== Starting server =======================");
      SetState(Starting);
      pthread_create(&_thread_ctrl, NULL, this->ThreadStartControlSocketServer, this);
      while(_sock_data_listen == -1)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      SetState(Started);
      debug("=============================== Started ===");
      break;
    case Stopped:
      debug("=== Stopping server ======================");
      SetState(Stopping);
      Disconnect();
      SetState(Stopped);
      debug("=============================== Stopped ===");
      break;
    case Destroyed:
      debug("=== Destroying server ====================");
      SetState(Destroying);
      Disconnect();
      SetState(Destroyed);
      debug("============================ Destroyed ===");
      break;
    default:
      throw new NetException("Illegal target state requested %d", nextState);

    }
  }
}

void* InterfaceServer::ThreadStartDataSocketServer(void * ctx){
  PTHREAD_NAME("smartscope.server.data-socket");
  ((InterfaceServer*)ctx)->DataSocketServer();
  return NULL;
}

void InterfaceServer::DataSocketServer() {
  sockaddr_in sa_cli;
  int length = 0, ret, sent;
  uint8_t smartScopeBuffer[BUF_SIZE];

  info("Waiting for data connection to be opened");
  socklen_t socklen = sizeof(sa_cli);
  if((_sock_data = accept(_sock_data_listen, (sockaddr *)&sa_cli, &socklen)) == -1)
    throw NetException("Failed to accept connection on data socket %s", strerror(errno));

  info("Connection accepted on data socket from %s:%d", inet_ntoa(sa_cli.sin_addr), ntohs(sa_cli.sin_port));

  length = DATA_SOCKET_BUFFER_SIZE;
  ret = setsockopt(_sock_data, SOL_SOCKET, SO_SNDBUF, &length, sizeof(int));
  if(ret)
    throw NetException("Data failed to set socket send buffer: %s", strerror(errno));

  ret = getsockopt(_sock_data, SOL_SOCKET, SO_SNDBUF, &length, &socklen);
  if(ret)
    throw NetException("Data failed to set socket send buffer: %s", strerror(errno));
  debug("Data socket size = %d bytes", length);

  while (_connected) {

    try
    {
      length = _scope->GetAcquisition(BUF_SIZE, smartScopeBuffer);
    }
    catch(ScopeIOException e)
    {
      info("USB error %s", e.what());
      Destroy();
      return;
    }

    sent = 0;
    while(sent < length) {
      if((sent += send(_sock_data, &smartScopeBuffer[sent], length - sent, 0)) == -1) {
        error("Failure while sending to socket: %s", strerror(errno));
        Stop();
        return;
      }
    }
  }
  info("Data thread aborted");
  Stop();
}

void* InterfaceServer::ThreadStartControlSocketServer(void * ctx){
  PTHREAD_NAME("smartscope.server.control-socket");
  try {
    ((InterfaceServer*)ctx)->ControlSocketServer();
  } catch(NetException e) {
    info("Network Exception thrown by control socket thread, stopping.\nMSG=[%s]", e.what());
    ((InterfaceServer*)ctx)->Stop();
  } catch(ScopeIOException e) {
    info("Scope IO Exception thrown by control socket thread, destroying.\nMSG=[%s]", e.what());
    ((InterfaceServer*)ctx)->Destroy();
  } catch(std::exception e) {
    info("Unknown exception thrown by control socket thread, stopping.\nMSG=[%s]", e.what());
    ((InterfaceServer*)ctx)->Stop();
  }

  return NULL;
}

void InterfaceServer::ControlSocketServer() {
  _disconnect_called = false;

  int ret;
  uint msg_buf_len = 0;
  uint msg_buf_offset = 0;
  Message *request, *response;
  ControllerMessage* ctrl_msg;

  sockaddr_in sa, sa_cli;
  socklen_t socklen = sizeof(sa);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string serial;
  uint32_t version;

  /* Start control server */
  if((_sock_ctrl_listen = InterfaceServer::StartServer("0")) == -1)
    throw NetException("Failed to control server socket");

  if(getsockname(_sock_ctrl_listen, (sockaddr *)&sa, &socklen) == -1)
    throw NetException("Couldn't get socket details %s", strerror(errno));
  info("Control socket listening on %s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  _port = ntohs(sa.sin_port);

  if((_sock_data_listen = InterfaceServer::StartServer("0")) == -1)
    throw NetException("Failed to start data server socket");

  socklen = sizeof(sa);
  if(getsockname(_sock_data_listen, (sockaddr *)&sa, &socklen) == -1)
    throw NetException("Couldn't get socket details %s", strerror(errno));
  info("Data socket listening on %s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  _port_data = ntohs(sa.sin_port);

  RegisterService();

  socklen = sizeof(sa_cli);
  if((_sock_ctrl = accept(_sock_ctrl_listen, (sockaddr *)&sa_cli, &socklen)) == -1)
    throw NetException("Failed to accept connection %s", strerror(errno));

  info("Connection accepted from %s:%d", inet_ntoa(sa_cli.sin_addr), ntohs(sa_cli.sin_port));
  UnregisterService();
  _connected = true;

  tx_buf = new uint8_t[BUF_SIZE];
  memset(tx_buf, 0, BUF_SIZE);
  response = (Message*)tx_buf;

  msg_buf = new uint8_t[MSG_BUF_SIZE];
  memset(msg_buf, 0, MSG_BUF_SIZE);

  while (_connected) {
    ret = recv(_sock_ctrl, &msg_buf[msg_buf_len], MSG_BUF_SIZE - msg_buf_len, 0);
    if(ret == -1)
      throw NetException("Failed to receive from socket: %s", strerror(errno));

    msg_buf_len += ret;

    if(msg_buf_len < sizeof(Message) - 1)
        continue;

    if (_connected) {
      while(msg_buf_len - msg_buf_offset > sizeof(Message) - 1) {
          request = (Message *)&msg_buf[msg_buf_offset];
          if(msg_buf_len - msg_buf_offset < request->length)
              goto copy_down;

          msg_buf_offset += request->length;
          response->cmd = request->cmd;

          switch (request->cmd) {
          case SERIAL:
            serial = _scope->GetSerial();
            if(serial.length() == 0)
                serial = std::string("0254301KA16");
            response->length = 11;
            memcpy(response->data, serial.c_str(), response->length);
            break;
          case PIC_FW_VERSION:
            version = _scope->GetPicFirmwareVersion();
            response->length = sizeof(version);
            memcpy(response->data, &version, response->length);
            break;
          case FLUSH:
            response->length = 0;
            _scope->FlushDataPipe();
            break;
          case FLASH_FPGA:
            response->length = 1;
            response->data[0] = 0xff;
            _scope->FlashFpga(request->length - HDR_SZ, request->data);
            break;
          case DISCONNECT:
            response->length = 0;
            info("Received Disconnect request from client");
            _scope->FlushDataPipe();
            Stop();
            return;
          case DATA:
            if(_thread_data)
              throw NetException("Should not mix data socket with data through control socket");
            response->length = *(uint16_t *)request->data;
            _scope->GetData(response->length, response->data, 0);
            break;
          case DATA_PORT:
            info("Starting data server...");
            pthread_create(&_thread_data, NULL, this->ThreadStartDataSocketServer, this);
            response->length = sizeof(_port_data);
            memcpy(response->data, &_port_data, response->length);
            break;
          case ACQUISITION:
            if(_thread_data)
              throw NetException("Should not mix data socket with data through control socket");
            do {
                response->length = _scope->GetAcquisition(BUF_SIZE - HDR_SZ, response->data);
            } while(response->length == 0);
            break;
          case SET:
            response->length = 0;
            ctrl_msg = (ControllerMessage *)request->data;
            _scope->SetControllerRegister(ctrl_msg->ctrl, ctrl_msg->addr, ctrl_msg->len, ctrl_msg->data);
            break;
          case GET:
            ctrl_msg = (ControllerMessage *)request->data;
            memcpy(response->data, ctrl_msg, sizeof(ControllerMessage));
            response->length = sizeof(ControllerMessage) + ctrl_msg->len;
            _scope->GetControllerRegister(ctrl_msg->ctrl, ctrl_msg->addr, ctrl_msg->len,
                                          ((ControllerMessage*)response->data)->data);
            break;
          default:
            info("Unsupported command %d", request->cmd);
            Stop();
            return;
          }
          if (response->length != 0)
          {
            response->length += sizeof(Message);
            uint sent = 0;
            while(sent < response->length)
                sent += send(_sock_ctrl, &tx_buf[sent], response->length - sent, 0);

          }
      }
copy_down:
      if(msg_buf_len != msg_buf_offset && msg_buf_offset > 0) {//unused bytes
        memcpy(msg_buf, &msg_buf[msg_buf_offset], msg_buf_len - msg_buf_offset);
      }
      msg_buf_len -= msg_buf_offset;
      msg_buf_offset = 0;
    }
    else
    {
      Stop();
      return;
    }
  }
}

void InterfaceServer::Disconnect() {

  pthread_t current_thread = pthread_self();
  if (!pthread_equal(current_thread, _thread_state))
    throw new NetException("State changing from wrong thread %p", &current_thread);

  if (_disconnect_called) {
      if (_connected)
          error("Wow this is bad!");
      return;
  }
  _disconnect_called = true;
  _connected = false;

  UnregisterService();
  debug("closing control thread/socket");
  CleanSocketThread(&_thread_ctrl, &_sock_ctrl_listen, &_sock_ctrl);
  debug("closing data thread/socket");
  CleanSocketThread(&_thread_data, &_sock_data_listen, &_sock_data);
  debug("Cleaning up message and tx buffers");
  delete[] msg_buf;
  delete[] tx_buf;
}

void InterfaceServer::CleanSocketThread(pthread_t* thread, int *listener_socket, int *socket)
{
  int err;

  if(*listener_socket > 0) {
    err = shutdown(*listener_socket, SHUT_RDWR);
    if(err)
      error("Failed to shut down listener socket: %s", strerror(errno));
    err = close(*listener_socket);
    if(err)
      error("Failed to close listener socket: %s", strerror(errno));
    *listener_socket = -1;
  }

  if(*socket > 0)
  {
    err = shutdown(*socket, SHUT_RDWR);
    if(err)
      error("Failed to close socket: %s", strerror(errno));
    err = close(*socket);
    if(err)
      error("Failed to close listener socket: %s", strerror(errno));
    *socket = -1;
  }

  if(*thread) {
    if((err = pthread_join_timeout(*thread, 5000))) {
      warn("Failed to join thread, canceling it: %s", strerror(err));
      pthread_cancel(*thread);
      if((err = pthread_join_timeout(*thread, 5000)))
        error("Failed to join thread, even after cancelling: %s", strerror(err));
    }
  } else {
    debug("Not joining uninitialized thread");
  }
  *thread = 0;
}

void InterfaceServer::Start()   { _stateRequested = Started; }
void InterfaceServer::Stop()    { _stateRequested = Stopped; }
void InterfaceServer::Destroy() { _stateRequested = Destroyed; }

void InterfaceServer::SetState(State state) {
  pthread_t current_thread = pthread_self();
  if (!pthread_equal(current_thread, _thread_state))
    throw new NetException("State changing from wrong thread %p", &current_thread);
  _state = state;
  if (_stateChanged != NULL)
    _stateChanged(this);
}
InterfaceServer::State InterfaceServer::GetState() { return _state; }

void InterfaceServer::RegisterService() {
  std::string name = "SmartScope []";
#ifdef DNSSD
  DNSServiceErrorType err = DNSServiceRegister(
        &_dnsService, 0, 0, name.c_str(), SERVICE_TYPE,
        NULL, NULL, htons(_port), 0, NULL, ServiceRegistered, this);
  if(err != kDNSServiceErr_NoError)
    error("Failure while registering service: %d", err);
#else
  int err;
  _avahi_entry_group = avahi_entry_group_new(_avahi_client, InterfaceServer::AvahiGroupChanged, this);
  err = avahi_entry_group_add_service(_avahi_entry_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, (AvahiPublishFlags)0,
                                name.c_str(), SERVICE_TYPE, NULL, NULL, _port, NULL);
  if(err)
    throw NetException("Failed to add service to avahi entry group: %s", avahi_strerror(err));

  err = avahi_entry_group_commit(_avahi_entry_group);
  if(err)
    throw NetException("Failed to commit entry group: %s", avahi_strerror(err));

#endif
  info("Zeroconf service registered");
}

#ifdef DNSSD
void InterfaceServer::ServiceRegistered(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    DNSServiceErrorType errorCode,
    const char *name,
    const char *regtype,
    const char *domain,
    void       *context
) {
  debug("Woop woop, service registerd with name %s, regtype %s, domain %s",
        name, regtype, domain);
}
#endif

#ifndef DNSSD
void InterfaceServer::AvahiCallback(AvahiClient *s, AvahiClientState state, void *data) {
  #define AVAHI_CLIENT_MSG "Avahi client state [%s]"
  switch(state)
  {
    case AVAHI_CLIENT_S_REGISTERING:
    debug(AVAHI_CLIENT_MSG, "REGISTERING"); break;
    case AVAHI_CLIENT_S_RUNNING:
    debug(AVAHI_CLIENT_MSG, "RUNNING");break;
    case AVAHI_CLIENT_S_COLLISION:
    debug(AVAHI_CLIENT_MSG, "COLLISION");break;
    case AVAHI_CLIENT_FAILURE:
    debug(AVAHI_CLIENT_MSG, "FAILURE");break;
    case AVAHI_CLIENT_CONNECTING:
    debug(AVAHI_CLIENT_MSG, "CONNECTING"); break;
  }
}
void InterfaceServer::AvahiGroupChanged(AvahiEntryGroup *g, AvahiEntryGroupState state, void* userdata )
{
  #define AVAHI_GROUP_MSG "Avahi group state [%s]"
  switch(state)
  {
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    debug(AVAHI_GROUP_MSG, "AVAHI_ENTRY_GROUP_UNCOMMITED"); break;
    case AVAHI_ENTRY_GROUP_REGISTERING:
    debug(AVAHI_GROUP_MSG, "AVAHI_ENTRY_GROUP_REGISTERING");break;
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
    debug(AVAHI_GROUP_MSG, "AVAHI_ENTRY_GROUP_ESTABLISHED");break;
    case AVAHI_ENTRY_GROUP_COLLISION:
    debug(AVAHI_GROUP_MSG, "AVAHI_ENTRY_GROUP_COLLISION");break;
    case AVAHI_ENTRY_GROUP_FAILURE:
    debug(AVAHI_GROUP_MSG, "AVAHI_ENTRY_GROUP_FAILURE");break;
  }
}

#endif

void InterfaceServer::UnregisterService() {
#ifdef DNSSD
  if(_dnsService != NULL)
    DNSServiceRefDeallocate(_dnsService);
#else
  if(_avahi_entry_group) {
    debug("Unregistered service");
    avahi_entry_group_reset(_avahi_entry_group);
  } else {
    debug("Service already unregistered");
  }
#endif

}

int InterfaceServer::StartServer(const char * port) {
  int sockfd;
  struct addrinfo hints, *servinfo, *p;
  int yes=1;
  int ret;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if ((ret = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
      error("getaddrinfo: %s\n", gai_strerror(ret));
      return -1;
  }

  for(p = servinfo; p != NULL; p = p->ai_next) {
      if ((sockfd = socket(p->ai_family, p->ai_socktype,
              p->ai_protocol)) == -1) {
          perror("server: socket");
          continue;
      }

      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
              sizeof(int)) == -1) {
          error("setsockopt");
          return -1;
      }

      if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
          close(sockfd);
          perror("server: bind");
          continue;
      }

      break;
  }

  freeaddrinfo(servinfo);

  if (p == NULL)  {
      error("server: failed to bind %s", strerror(errno));
      return -1;
  }

  if (listen(sockfd, 1) == -1) {
      error("listen %s", strerror(errno));
      exit(1);
  }

  return sockfd;
}

}
