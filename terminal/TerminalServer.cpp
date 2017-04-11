#include "ClientConnection.hpp"
#include "ConsoleUtils.hpp"
#include "CryptoHandler.hpp"
#include "FlakyFakeSocketHandler.hpp"
#include "Headers.hpp"
#include "ServerConnection.hpp"
#include "SocketUtils.hpp"
#include "UnixSocketHandler.hpp"
#include "ProcessHelper.hpp"
#include "IdPasskeyHandler.hpp"

#include "simpleini/SimpleIni.h"

#include <errno.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if __APPLE__
#include <util.h>
#elif __FreeBSD__
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>
#else
#include <pty.h>
#endif

#include "ETerminal.pb.h"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

map<string, int64_t> idPidMap;
shared_ptr<ServerConnection> globalServer;

void halt();

#define FAIL_FATAL(X)                                              \
  if ((X) == -1) {                                                 \
    LOG(FATAL) << "Error: (" << errno << "): " << strerror(errno); \
  }

DEFINE_int32(port, 0, "Port to listen on");
DEFINE_string(idpasskey, "", "If set, uses IPC to send a client id/key to the server daemon");
DEFINE_string(idpasskeyfile, "", "If set, uses IPC to send a client id/key to the server daemon from a file");
DEFINE_bool(daemon, false, "Daemonize the server");
DEFINE_string(cfgfile, "", "Location of the config file");

thread* idPasskeyListenerThread = NULL;

thread* terminalThread = NULL;
void runTerminal(shared_ptr<ServerClientConnection> serverClientState,
                 int masterfd) {
  string disconnectBuffer;

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (1024)
  char b[BUF_SIZE];

  while (run) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    FD_SET(masterfd, &rfd);
    int maxfd = masterfd;
    int serverClientFd = serverClientState->getSocketFd();
    if (serverClientFd > 0) {
      FD_SET(serverClientFd, &rfd);
      maxfd = max(maxfd, serverClientFd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // Check for data to receive; the received
      // data includes also the data previously sent
      // on the same master descriptor (line 90).
      if (FD_ISSET(masterfd, &rfd)) {
        // Read from fake terminal and write to server
        memset(b, 0, BUF_SIZE);
        int rc = read(masterfd, b, BUF_SIZE);
        if (rc > 0) {
          // VLOG(2) << "Sending bytes: " << int(b) << " " << char(b) << " "
          // << serverClientState->getWriter()->getSequenceNumber();
          char c = et::PacketType::TERMINAL_BUFFER;
          serverClientState->writeMessage(string(1, c));
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);
          serverClientState->writeProto(tb);
        } else {
          LOG(INFO) << "Terminal session ended";
          run = false;
          globalServer->removeClient(serverClientState->getId());
          break;
        }
      }

      if (serverClientFd > 0 && FD_ISSET(serverClientFd, &rfd)) {
        while (serverClientState->hasData()) {
          string packetTypeString;
          if (!serverClientState->readMessage(&packetTypeString)) {
            break;
          }
          char packetType = packetTypeString[0];
          switch (packetType) {
            case et::PacketType::TERMINAL_BUFFER: {
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  serverClientState->readProto<et::TerminalBuffer>();
              const string& s = tb.buffer();
              // VLOG(2) << "Got byte: " << int(b) << " " << char(b) << " " <<
              // serverClientState->getReader()->getSequenceNumber();
              FATAL_FAIL(writeAll(masterfd, &s[0], s.length()));
              break;
            }
            case et::PacketType::KEEP_ALIVE: {
              // Echo keepalive back to client
              VLOG(1) << "Got keep alive";
              char c = et::PacketType::KEEP_ALIVE;
              serverClientState->writeMessage(string(1, c));
              break;
            }
            case et::PacketType::TERMINAL_INFO: {
              VLOG(1) << "Got terminal info";
              et::TerminalInfo ti =
                  serverClientState->readProto<et::TerminalInfo>();
              winsize tmpwin;
              tmpwin.ws_row = ti.row();
              tmpwin.ws_col = ti.column();
              tmpwin.ws_xpixel = ti.width();
              tmpwin.ws_ypixel = ti.height();
              ioctl(masterfd, TIOCSWINSZ, &tmpwin);
              break;
            }
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType) << endl;
          }
        }
      }
    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what();
      cerr << "Error: " << re.what();
      serverClientState->closeSocket();
      // If the client disconnects the session, it shuoldn't end
      // because the client may be starting a new one.  TODO: Start a
      // timer which eventually kills the server.

      // run=false;
    }
  }
  {
    string id = serverClientState->getId();
    serverClientState.reset();
    globalServer->removeClient(id);
  }
}

void startTerminal(shared_ptr<ServerClientConnection> serverClientState,
                   InitialPayload payload) {
  const TerminalInfo& ti = payload.terminal();
  winsize win;
  win.ws_row = ti.row();
  win.ws_col = ti.column();
  win.ws_xpixel = ti.width();
  win.ws_ypixel = ti.height();
  for (const string& it : payload.environmentvar()) {
    size_t equalsPos = it.find("=");
    if (equalsPos == string::npos) {
      LOG(FATAL) << "Invalid environment variable";
    }
    string name = it.substr(0, equalsPos);
    string value = it.substr(equalsPos + 1);
    setenv(name.c_str(), value.c_str(), 1);
  }

  int masterfd;
  std::string terminal = getTerminal();

  pid_t pid = forkpty(&masterfd, NULL, NULL, &win);
  switch (pid) {
    case -1:
      FAIL_FATAL(pid);
    case 0: {
      // child
      VLOG(1) << "Closing server in fork" << endl;
      // Close server on client process
      globalServer->close();
      globalServer.reset();
      string id = serverClientState->getId();
      if (idPidMap.find(id) == idPidMap.end()) {
        LOG(FATAL) << "Error: PID for ID not found";
      }
      passwd* pwd = getpwuid(idPidMap[id]);
      setuid(pwd->pw_uid);
      seteuid(pwd->pw_uid);
      setgid(pwd->pw_gid);
      setegid(pwd->pw_gid);
      if (pwd->pw_shell) {
        terminal = pwd->pw_shell;
      }
      setenv("SHELL", terminal.c_str(), 1);

      const char *homedir = pwd->pw_dir;
      setenv("HOME", homedir, 1);
      setenv("USER", pwd->pw_name, 1);
      setenv("LOGNAME", pwd->pw_name, 1);
      setenv("PATH", "/usr/local/bin:/bin:/usr/bin", 1);
      chdir(pwd->pw_dir);

      VLOG(1) << "Child process " << terminal << endl;
      execl(terminal.c_str(), terminal.c_str(), NULL);
      exit(0);
      break;
    }
    default:
      // parent
      cout << "pty opened " << masterfd << endl;
      terminalThread = new thread(runTerminal, serverClientState, masterfd);
      break;
  }
}

class TerminalServerHandler : public ServerConnectionHandler {
  virtual bool newClient(shared_ptr<ServerClientConnection> serverClientState) {
    InitialPayload payload = serverClientState->readProto<InitialPayload>();
    startTerminal(serverClientState, payload);
    return true;
  }
};

bool doneListening=false;

int main(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  FLAGS_logbufsecs = 0;
  FLAGS_logbuflevel = google::GLOG_INFO;
  srand(1);

  if (FLAGS_cfgfile.length()) {
    // Load the config file
    CSimpleIniA ini(true, true, true);
    SI_Error rc = ini.LoadFile(FLAGS_cfgfile.c_str());
    if (rc == 0) {
      if (FLAGS_port == 0) {
        const char* portString = ini.GetValue("Networking", "Port", NULL);
        if (portString) {
          FLAGS_port = stoi(portString);
        }
      }
    } else {
      LOG(FATAL) << "Invalid config file: " << FLAGS_cfgfile;
    }
  }

  if (FLAGS_port == 0) {
    FLAGS_port = 2022;
  }

  if (FLAGS_idpasskey.length() > 0 || FLAGS_idpasskeyfile.length() > 0) {
    string idpasskey = FLAGS_idpasskey;
    if (FLAGS_idpasskeyfile.length() > 0) {
      // Check for passkey file
      std::ifstream t(FLAGS_idpasskeyfile.c_str());
      std::stringstream buffer;
      buffer << t.rdbuf();
      idpasskey = buffer.str();
      // Trim whitespace
      idpasskey.erase(idpasskey.find_last_not_of(" \n\r\t") + 1);
      // Delete the file with the passkey
      remove(FLAGS_idpasskeyfile.c_str());
    }
    idpasskey += '\0';
    IdPasskeyHandler::send(idpasskey);

    return 0;
  }

  if (FLAGS_daemon) {
    ProcessHelper::daemonize();
  }

  std::shared_ptr<UnixSocketHandler> serverSocket(new UnixSocketHandler());

  LOG(INFO) << "Creating server";

  globalServer = shared_ptr<ServerConnection>(new ServerConnection(
      serverSocket, FLAGS_port,
      shared_ptr<TerminalServerHandler>(new TerminalServerHandler())));
  idPasskeyListenerThread = new thread(IdPasskeyHandler::runServer, &doneListening);
  globalServer->run();
}

void halt() {
  LOG(INFO) << "Shutting down server" << endl;
  doneListening = true;
  globalServer->close();
  LOG(INFO) << "Waiting for server to finish" << endl;
  sleep(3);
  exit(0);
}
