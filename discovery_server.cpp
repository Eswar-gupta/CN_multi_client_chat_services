#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>

#define DISCOV_PORT 6000
#define MAX_MSG     (1 << 20)

using namespace std;
struct UserRecord
{
    string password;
    string ip;
    int    port;
};

unordered_map<string, UserRecord> user_db;
mutex db_mutex;

bool send_packet(int fd, const string& msg)
{
    uint32_t net_len = htonl(msg.size());
    if (send(fd, &net_len, sizeof(net_len), MSG_NOSIGNAL) <= 0)
        return false;
    if (!msg.empty())
        if (send(fd, msg.data(), msg.size(), MSG_NOSIGNAL) <= 0)
            return false;
    return true;
}

string recv_packet(int fd)
{
    uint32_t net_len = 0;
    int r = recv(fd, &net_len, sizeof(net_len), MSG_WAITALL);
    if (r <= 0) return "";

    uint32_t len = ntohl(net_len);
    if (len == 0 || len > MAX_MSG) return "";

    vector<char> buf(len);
    r = recv(fd, buf.data(), len, MSG_WAITALL);
    if (r <= 0) return "";

    return string(buf.begin(), buf.end());
}

void handle_client(int client_fd)
{
    string command = recv_packet(client_fd);
    // Format: REGISTER -> username -> password -> port
    if (command == "REGISTER")
    {
        string username = recv_packet(client_fd);
        string password = recv_packet(client_fd);
        string port_str = recv_packet(client_fd);

        if (username.empty() || password.empty())
        {
            send_packet(client_fd, "ERROR");
            close(client_fd);
            return;
        }

        int port = 0;
        try { port = stoi(port_str); }
        catch (...) { port = 0; }

        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        getpeername(client_fd,
                    reinterpret_cast<sockaddr*>(&addr),
                    &len);
        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf));

        lock_guard<mutex> lock(db_mutex);

        if (user_db.count(username))
        {
            // Username already registered
            send_packet(client_fd, "USERNAME_TAKEN");
        }
        else
        {
            user_db[username] = { password, string(ip_buf), port };
            send_packet(client_fd, "REGISTERED");
            cout << "[Discovery] Registered: " << username
                 << " @ " << ip_buf << ":" << port << endl;
        }
    }

    // Format: LOGIN -> username -> password
    else if (command == "LOGIN")
    {
        string username = recv_packet(client_fd);
        string password = recv_packet(client_fd);

        lock_guard<mutex> lock(db_mutex);

        auto it = user_db.find(username);

        if (it == user_db.end())
        {
            send_packet(client_fd, "NO_SUCH_USER");
        }
        else if (it->second.password != password)
        {
            send_packet(client_fd, "INVALID");
        }
        else
        {
            send_packet(client_fd, "VALID");
        }
    }

    // Format: LOOKUP -> username
    else if (command == "LOOKUP")
    {
        string username = recv_packet(client_fd);

        lock_guard<mutex> lock(db_mutex);

        auto it = user_db.find(username);
        if (it == user_db.end())
        {
            send_packet(client_fd, "NOT_FOUND");
        }
        else
        {
            string result = it->second.ip + ":" +
                            to_string(it->second.port);
            send_packet(client_fd, result);
        }
    }

    else if (command == "LIST")
    {
        lock_guard<mutex> lock(db_mutex);

        string result;
        for (const auto& kv : user_db)
        {
            if (!result.empty()) result += ",";
            result += kv.first;
        }

        if (result.empty()) result = "(none)";
        send_packet(client_fd, result);
    }

    else
    {
        send_packet(client_fd, "UNKNOWN_COMMAND");
    }

    close(client_fd);
}

int main()
{
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(DISCOV_PORT);

    if (bind(server_fd,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0)
    {
        perror("bind"); return 1;
    }

    if (listen(server_fd, 20) < 0)
    {
        perror("listen"); return 1;
    }

    cout << "Discovery Server running on port "
         << DISCOV_PORT << "..." << endl;

    for (int i = 1; i <= 15; i++)
    {
        string uname = "user" + to_string(i);
        string pass  = "pass" + to_string(i);
        user_db[uname] = { pass, "127.0.0.1", 5000 };
    }
    cout << "[Discovery] Pre-registered 15 test users (user1-user15 / pass1-pass15)" << endl;

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);

        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        thread(handle_client, client_fd).detach();
    }

    close(server_fd);
    return 0;
}
