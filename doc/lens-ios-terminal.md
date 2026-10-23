# ns_lens iOS 终端实现计划

## 目标与当前边界

ns_lens 的 iOS 应用继续使用 `target = "exec"`：Lens 本身提前编译、签名，
仓库代码作为可查看、可编辑的数据下载。当前版本提供 Keychain SSH 身份、
主机指纹确认、libgit2 仓库操作与共享编辑器。桌面已有的 `terminal.ns` 保留
ANSI 解析、字符网格、滚动历史、选择、复制粘贴和按键重复；iOS 的本地
`os_pty_open` 仍返回不可用，不伪装成已启动 shell。

## 进度

- 阶段一、二已完成：`terminal.ns` 通过 `terminal_session_*` 在桌面 pty 与
  iOS SSH 之间分支；SSH 后端是 `lib/src/os.ssh.c`，接口为 `os` 模块中的
  `os_ssh_*`（见下文“实际接口”）。
- 阶段三部分完成：终端面板显示连接状态，标题栏有 host（编辑主机）和
  connect（重连）按钮，底部有 esc/tab/ctrl/方向键/^C/paste 辅助键行，系统
  键盘与输入法提交的 UTF-8 直接发送。尚未完成：会话切换、tmux 附着、
  后台/锁屏/网络切换的真机验证。
- 阶段四、五未开始。

## 实际接口

没有新增 `lib/ssh.ns` 模块：SSH 终端与嵌入式 Git 共用同一份 libssh2/OpenSSL、
Keychain 身份与主机指纹记录，因此放在 `os` 中，并且只在调用
`os_git_embedded()` 的 iOS 应用里可用；其他构建返回不可用。

```ns
lit OS_SSH_FAILED = -1
lit OS_SSH_CLOSED = 0
lit OS_SSH_CONNECTING = 1
lit OS_SSH_VERIFYING = 2
lit OS_SSH_OPEN = 3
ref fn os_ssh_open(host: str, port: i32, user: str, folder: str, cols: i32, rows: i32) i32
ref fn os_ssh_state(handle: i32) i32
ref fn os_ssh_error(handle: i32) str
ref fn os_ssh_read(handle: i32, capacity: i32) str
ref fn os_ssh_write(handle: i32, data: str) i32
ref fn os_ssh_resize(handle: i32, cols: i32, rows: i32) i32
ref fn os_ssh_close(handle: i32)
```

与原设计的差异：读写沿用 `os_pty_*` 的 `str`，输出中的 NUL 字节被丢弃
（终端本身也忽略 NUL）；每个会话一个分离的 worker 线程，应用线程只与两个
有界缓冲交换字节（输入 256 KiB，输出 1 MiB，输出满时停止读取形成背压）。
已被通道接收的输入立即出队，会话结束时未发送的输入被丢弃，不会重放。
仅支持公钥认证；远程目录通过 `cd -- '<folder>'; exec "$SHELL" -l` 进入，
目录不存在时仍打开 shell。主机设置保存在应用数据目录的 `ssh_terminal`，
与 Git remote 无关。

`sh scripts/test_embedded_git.sh` 在本机临时启动一个只监听 loopback 的
非特权 sshd，用 ASan/UBSan 验证：引号目录、连接中排队输入、PTY 尺寸与
调整、超过 16 KiB 的连续输出、退出码、拒绝主机指纹、缺少密钥、用户被拒、
端口拒绝连接以及句柄槽位复用。

建议先实现 **SSH 远程终端**，再补应用内的有限本地命令。完整的开发 shell、
编译器、包管理器放在用户的 Mac/Linux 开发机上执行，手机负责显示和编辑。
`exec` 是 Lens 的构建模式，不代表 iOS 可以执行 clone 下来的任意原生程序。

## 阶段一：抽出终端传输层

在 `nscode/ns_lens/src/terminal.ns` 的现有显示模型外增加一个具体的 session
结构，记录 `kind`、句柄、状态、待发送队列、错误和远程工作目录。用少量
`terminal_session_read/write/resize/close` 函数按 local/ssh 分支调用后端。
桌面 local 分支继续走 `os_pty_*`，ANSI 解析器和渲染不变。

验收：桌面原有终端测试全部通过；模拟传输可验证分片输出、部分写入、
断开后不重发命令、关闭后释放句柄。

## 阶段二：新增 lib/ssh.ns 和 libssh2 后端

复用当前已构建的 libssh2/OpenSSL。把 Git 使用的 Keychain 身份和 SSH
主机指纹检查抽成 Apple 通用 SSH 服务，Git 与终端共享实现、各自持有连接。
Git 仓库服务器和开发机可能不同，终端连接表单必须单独填写 host、port、
username、初始目录，不能从 Git remote 推断成同一台机器。

新增 FFI 安全的具体接口：

```ns
ref fn ssh_open(host: str, port: i32, user: str) i32
ref fn ssh_poll(session: i32) i32
ref fn ssh_request_terminal(session: i32, cols: i32, rows: i32) i32
ref fn ssh_read(session: i32, output: [u8], capacity: i32) i32
ref fn ssh_write(session: i32, input: [u8], size: i32) i32
ref fn ssh_resize(session: i32, cols: i32, rows: i32) i32
ref fn ssh_close(session: i32)
```

这些接口是待实现的设计，不是现有 API。连接、握手、鉴权、PTY 申请、shell
启动均使用非阻塞状态机，由单一后台 worker 持有 libssh2 session。处理
`LIBSSH2_ERROR_EAGAIN`、收发背压、超时、取消、EOF 和退出码。用字节数组跨越
FFI，保留 UTF-8 分片、转义序列和 NUL。不要让网络调用阻塞 UI 帧。

PTY 使用 `xterm-256color`，之后调用 `libssh2_channel_shell`；旋转、分屏和
软件键盘改变可见行列时发送 `libssh2_channel_request_pty_size_ex`。
输入 Ctrl+C、Tab、方向键、组合键均发送对应终端字节序列。

验收：临时 SSH 测试服务器覆盖正确/错误/加密密钥、未知/变化的主机指纹、
连接超时、部分写、窗口调整、远程退出；可交互运行 shell、top 和 vim。

## 阶段三：移动端交互与会话恢复

在浮动终端面板放连接状态、连接/断开和会话切换；顶部菜单仍只放下拉菜单。
键盘上方增加 Esc、Tab、Ctrl、方向键等辅助键；补齐中文、粘贴、多点触摸、
长按选择、滚动历史，以及 iPhone/iPad 横竖屏和外接键盘行为。

进入后台时保存终端显示状态，关闭或挂起网络；回前台显示真实连接状态。
iOS 不保证后台持续运行，不能承诺 SSH 永久在线。提供可选的远程 tmux
会话附着，自动重连只恢复连接，不自动重放可能产生副作用的命令。

验收：真机后台/锁屏/网络切换后没有重复执行、丢失未发送输入或假在线状态。

## 阶段四：编辑器与远程工作目录联动

第一版明确区分手机本地仓库与开发机远程目录。终端不会自动在手机的本地
文件上执行命令。用户可选择：

- Git 同步：手机提交并 push，开发机 fetch/pull，构建后通过 Git 带回修改。
- 后续 SFTP 模式：在连接配置中明确本地根目录与远程根目录的映射。保存前
  比较基准哈希，远端修改时提示冲突；上传采用临时文件加原子重命名。

禁止默认双向覆盖。把编译日志中的远程路径映射回本地编辑器时，校验路径
处于配置根目录内，拒绝 `..` 越界。SFTP 需要独立的冲突和断网恢复测试。

## 阶段五：可选的离线本地命令

如果需要离线操作，新增进程内命令注册表，优先实现 `pwd`、`ls`、`cd`、
`cat`、`mkdir`、`cp`、`mv` 和现有 libgit2 操作。所有命令限定在用户选择的
工作区和应用容器中，用受控流连接输入输出。可评估 ios_system 中已有的
命令移植，但需要审核其线程、全局状态和退出行为。

Nano Script 本地执行可另做嵌入式 ns_cpu 会话，设置取消、内存/输出配额和
模块权限。它不等同于本地 Unix shell，也不应承诺运行任意下载的机器码。
若发布到 App Store，应按实际提供的代码执行能力核对审核规则。

## 建议交付顺序

1. 阶段一、二：能稳定连接开发机并交互执行命令。
2. 阶段三：完成手机和 iPad 的日常使用体验与后台恢复。
3. 阶段四：先提供显式 Git 同步，再按需要增加 SFTP。
4. 阶段五：作为独立功能追加，不阻塞远程终端交付。

## 依据

- [libssh2 官方交互 shell 示例](https://libssh2.org/examples/ssh2.html)
- [libssh2 PTY 尺寸调整](https://libssh2.org/libssh2_channel_request_pty_size.html)
- [ios_system 项目说明](https://github.com/holzschu/ios_system)
- [Apple App Review Guidelines，2.5.2](https://developer.apple.com/app-store/review/guidelines/)
