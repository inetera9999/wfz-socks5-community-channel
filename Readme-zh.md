# socks5-fz

一个用 **C 语言** 编写的轻量级 **SOCKS5 代理（服务端 + 客户端）**，基于 **epoll（水平触发 LT）** 与 **非阻塞 Socket**，由单线程事件循环和 TCP 隧道状态机驱动。

本项目是 [bhhbazinga/socks5](https://github.com/bhhbazinga/socks5) 的增强分支（by **wFZ**），在原始服务端之外，新增了 **隧道复用 / keep-alive 机制** 和一个配套的 **SOCKS5 客户端**（`clientdemo`）。

> 仅支持 UNIX/Linux 平台（依赖 `epoll`、POSIX Socket）。

---

## 功能特性

### SOCKS5 服务端（`socks5`）
- 单线程事件循环：`epoll`（水平触发）+ 非阻塞 Socket
- 无认证 / 用户名密码认证（RFC 1929）
- 通过 `getaddrinfo` 支持 IPv4 / IPv6 / 域名解析
- `CONNECT` 命令
- **双监听端口：**
  - `-p`：标准 SOCKS5 代理端口（供代理软件 / 浏览器 / `curl` 使用）
  - `-P`：私有隧道端口（与内置客户端配对）
- **隧道复用（wFZ 扩展）：** 空闲的已建立隧道通过连接池与新请求配对，避免每个请求都新建 TCP 连接
- HTTP `Connection: keep-alive` 探测（`parsingHeader`），决定隧道 Socket 是否保活并复用

### SOCKS5 客户端（`clientdemo`）
- 配套客户端 / *中介（mediator）*，主动连接服务端的私有隧道端口（`-P`）
- 非阻塞 `connect` + `getaddrinfo` 多地址回退
- 与服务端连接池配对，形成可复用的隧道

### 公共基础设施
- `buff.c` — 可增长的 I/O 缓冲，处理 TCP 粘包 / 拆包
- `list.h` — Linux 内核风格的侵入式双向链表
- `Utils.c` — 连接池管理：AppList（活跃隧道）/ waitList（等待配对隧道）/ FdList（原始 fd）
- `ventry.h` — `container_of` 风格宏（`vbody_entry`），兼容 GCC 与非 GCC 编译器

---

## 构建

```sh
make
```

在项目根目录生成两个二进制：

| 二进制 | 说明 |
|--------|------|
| `socks5`     | SOCKS5 代理服务端 |
| `clientdemo` | SOCKS5 隧道客户端 / 中介 |

清理：

```sh
make clean
```

需要 `gcc` 及标准 POSIX 头文件。编译选项：`-Wall -g -std=c99`。

---

## 使用方法

### 服务端

```
Usage:
  -a <ip>  绑定地址
  -p <port>       代理监听端口
  -P <port>       私有隧道监听端口（可选，用于客户端配对）
  -u <username>   用户名（可选，启用认证）
  -k <password>   密码（可选，启用认证）
```

```sh
# 基础代理，无认证
./socks5 -a 0.0.0.0 -p 1080

# 用户名密码认证
./socks5 -a 0.0.0.0 -p 1080 -u abc123 -k qwe123

# 为内置客户端开启私有隧道端口
./socks5 -a 0.0.0.0 -p 1080 -P 6080
```

### 客户端（`clientdemo`）

```
Usage:
  -a <ip>  服务器地址
  -p <port>       连接服务器的媒体/私有端口
  -P <port>       远端端口（可选）
  -u <username>   用户名（可选）
  -k <password>   密码（可选）
```

```sh
./clientdemo -a 192.168.1.40 -p 1080 -u abc123 -k qwe123
```

---

## 快速测试

```sh
# 1. 启动服务端
./socks5 -a 0.0.0.0 -p 6080

# 2. 通过代理访问
curl --socks5 127.0.0.1:6080 http://www.baidu.com
curl --socks5 127.0.0.1:6080 http://192.168.150.128:8090
```

---

## 架构

```
                          ┌───────────────────────────────┐
 客户端/代理软件 ────────▶ │  socks5 服务端（epoll LT）     │
 （SOCKS5 CONNECT）        │  状态机：                      │
                          │   open → auth → request        │
                          │   → connecting → connected     │
                          │   → waitting（复用）           │
                          └──────────────┬────────────────┘
                                         │ 私有隧道端口（-P）
                          ┌──────────────▼────────────────┐
      clientdemo ────────▶│  连接池 / 配对                 │
    （中介 mediator）      │  AppList ↔ waitList           │
                          └───────────────────────────────┘
```

### 隧道状态机

| 状态 | 说明 |
|------|------|
| `open_state` | 收到客户端 SOCKS5 握手（`VER`/`NMETHODS`） |
| `auth_state` | 用户名密码协商（启用认证时） |
| `request_state` | 解析 CONNECT 请求（`CMD`/`ATYP`/地址/端口） |
| `connecting_state` | 向远端目标发起（非阻塞）连接 |
| `connected_state` | 客户端与远端之间的双向数据转发 |
| `tunnel_waitting` |（wFZ）隧道写完数据后被保活，等待与下一个请求配对 |

每个隧道对（`priv_sock` ↔ `remote_sock`）由 `AppList` / `waitList` 跟踪；`waitLinePairing()` 在每轮 epoll 之后运行，将空闲隧道与待处理请求配对，复用已建立的 TCP 连接。

---

## 项目结构

```
├── socks5.c        # SOCKS5 代理服务端（主入口、状态机、隧道池）
├── clientdemo.c    # SOCKS5 隧道客户端 / 中介
├── mysocks5.h      # 共享头文件：协议结构体、tunnel/sock/server 类型
├── util.h          # 基于链表的连接池 API
├── buff.c / buff.h # 可增长的 I/O 缓冲
├── Utils.c         # AppList / waitList / FdList 连接池实现
├── list.h          # 侵入式双向链表
├── ventry.h        # container_of 宏（vbody_entry）
├── leveltrg_demo.c # epoll 水平触发模式的教学示例
├── Makefile
└── screenshot/     # 使用截图
```

---

## TODO

- DNS 异步解析与缓存（当前 `getaddrinfo` 阻塞）
- 加密传输
- `BIND` / `ASSOCIATE` 命令支持
- 连接池条目的边界检查

---

## 许可证与致谢

[bhhbazinga/socks5](https://github.com/bhhbazinga/socks5) 的增强分支（wFZ）。详见原项目许可证。