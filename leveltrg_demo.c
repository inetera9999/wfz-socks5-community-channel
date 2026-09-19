// 水平触发的DEMO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

#define MAX_EVENTS 1024  // 最大事件数
#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int listen_fd, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 1. 创建监听socket
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 设置端口复用（避免bind时Address already in use）
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // 绑定地址和端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡
    server_addr.sin_port = htons(PORT);
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // 开始监听
    if (listen(listen_fd, 10) == -1) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d...\n", PORT);

    // 2. 创建epoll实例
    if ((epoll_fd = epoll_create1(0)) == -1) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    // 3. 向epoll添加监听socket（水平触发，默认不设置EPOLLET）
    struct epoll_event event;
    event.events = EPOLLIN;  // 关注可读事件（新连接）
    event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        perror("epoll_ctl add listen_fd failed");
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[MAX_EVENTS];  // 存储触发的事件
    while (1) {
        // 4. 等待事件触发（超时时间-1表示永久阻塞）
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failed");
            exit(EXIT_FAILURE);
        }

        // 处理所有触发的事件
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            // 情况1：监听socket触发可读事件（有新连接）
            if (fd == listen_fd) {
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd == -1) {
                    perror("accept failed");
                    continue;
                }
                printf("New connection from %s:%d (fd=%d)\n",
                       inet_ntoa(client_addr.sin_addr),
                       ntohs(client_addr.sin_port),
                       client_fd);

                // 将新客户端socket添加到epoll（水平触发，不设置EPOLLET）
                event.events = EPOLLIN;  // 关注客户端发送的数据（可读事件）
                event.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                    perror("epoll_ctl add client_fd failed");
                    close(client_fd);
                }
            }
            // 情况2：客户端socket触发可读事件（有数据发送）
            else if (events[i].events & EPOLLIN) {
                char buffer[BUFFER_SIZE];
                ssize_t bytes_read = read(fd, buffer, BUFFER_SIZE - 1);  // 读取部分数据（演示水平触发）
                if (bytes_read == -1) {
                    perror("read failed");
                    close(fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);  // 从epoll中移除
                } else if (bytes_read == 0) {  // 客户端关闭连接
                    printf("Client fd=%d disconnected\n", fd);
                    close(fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                } else {  // 读取到数据
                    buffer[bytes_read] = '\0';
                    printf("Received from fd=%d: %s (read %zd bytes)\n", fd, buffer, bytes_read);
                    // 水平触发特性：即使未读完所有数据，下次epoll_wait仍会通知该fd的可读事件
                }
            }
        }
    }

    // 清理资源（实际服务端通常不退出）
    close(listen_fd);
    close(epoll_fd);
    return 0;
}