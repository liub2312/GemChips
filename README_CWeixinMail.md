# CWeixinMail — 微信企业邮箱自动抓取组件

## 功能概述

`CWeixinMail` 是一个 C++17 共享库（`.so`），通过 **IMAP over SSL** 协议自动连接
微信企业邮箱（`imap.exmail.qq.com:993`），后台线程按设定间隔自动拉取并缓存邮件，
对外提供 `List` / `Get` / `Del` 三个核心接口，同时导出 C-ABI 供 Python / Go /
Java 等语言通过 FFI / `dlopen` 调用。

---

## 目录结构

```
.
├── CMakeLists.txt
├── include/
│   └── CWeixinMail.h        # 头文件（C++ 类 + C-ABI 声明）
├── src/
│   └── CWeixinMail.cpp      # 实现
└── examples/
    └── demo.cpp             # 使用示例
```

---

## 依赖

| 依赖 | 版本要求 | 说明 |
|------|---------|------|
| libcurl | ≥ 7.50，需带 IMAP + SSL 支持 | 邮件收取 |
| C++17 编译器 | GCC ≥ 7 / Clang ≥ 5 | |
| pthreads | 系统自带 | 后台轮询线程 |

```bash
# Ubuntu / Debian
sudo apt install libcurl4-openssl-dev

# CentOS / RHEL
sudo yum install libcurl-devel
```

---

## 编译

### 使用 CMake（推荐）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# 产物：libweixinmail.so  demo
```

### 手动编译（单命令）

```bash
# 仅编译 .so
g++ -std=c++17 -fPIC -shared -O2 \
    -Iinclude src/CWeixinMail.cpp \
    -lcurl -lpthread \
    -o libweixinmail.so

# 编译 demo（链接 .so）
g++ -std=c++17 -Iinclude examples/demo.cpp \
    -L. -lweixinmail -Wl,-rpath,. -lpthread \
    -o demo
```

---

## 快速上手（C++ API）

```cpp
#include "CWeixinMail.h"
using namespace weixin;

CWeixinMail mail;

MailConfig cfg;
cfg.username          = "user@company.com";
cfg.password          = "your_imap_auth_token";
cfg.poll_interval_sec = 60;  // 每 60 秒自动拉取

// 新邮件到达回调
mail.SetNewMailCallback([](const std::vector<MailInfo>& newMails) {
    for (auto& m : newMails)
        printf("New: [%s] %s\n", m.uid.c_str(), m.subject.c_str());
});

mail.Init(cfg);   // 初始化并首次拉取
mail.Start();     // 启动后台自动轮询

// --- 列出目录 ---
auto dirs = mail.List("");          // mailbox="" 返回目录列表

// --- 列出 INBOX 邮件 ---
auto mails = mail.List("INBOX");
for (auto& m : mails)
    printf("[%s] %s  from %s\n", m.uid.c_str(), m.subject.c_str(), m.from.c_str());

// --- 获取邮件内容 ---
MailContent c = mail.Get("INBOX", "42");
printf("%s\n", c.body_text.c_str());

// --- 删除邮件 ---
mail.Del("INBOX", "42");

// --- 删除整个目录（慎用！） ---
// mail.Del("SomeFolder");

mail.Stop();
```

---

## C-ABI 接口（供 dlopen / FFI）

```c
void*       cwm_create(void);
void        cwm_destroy(void* handle);
int         cwm_init(void* handle,
                     const char* host,   // NULL → imap.exmail.qq.com
                     int         port,   // 0 → 993
                     const char* username,
                     const char* password,
                     int         poll_interval_sec);
void        cwm_start(void* handle);
void        cwm_stop(void* handle);
void        cwm_poll_now(void* handle);

// 返回 JSON 字符串（堆分配，调用者需 free()）
char*       cwm_list(void* handle, const char* mailbox);
char*       cwm_get(void* handle,  const char* mailbox, const char* uid);
int         cwm_del(void* handle,  const char* mailbox, const char* uid);
const char* cwm_last_error(void* handle);
```

### Python 调用示例

```python
import ctypes, json

lib = ctypes.CDLL("./libweixinmail.so")
lib.cwm_list.restype  = ctypes.c_char_p
lib.cwm_get.restype   = ctypes.c_char_p
lib.cwm_last_error.restype = ctypes.c_char_p

h = lib.cwm_create()
lib.cwm_init(h, None, 0,
             b"user@company.com",
             b"your_password",
             60)
lib.cwm_start(h)

# 列出 INBOX
raw = lib.cwm_list(h, b"INBOX")
mails = json.loads(raw)
for m in mails:
    print(m["uid"], m["subject"], m["from"])

# 获取邮件内容
content_raw = lib.cwm_get(h, b"INBOX", mails[0]["uid"].encode())
content = json.loads(content_raw)
print(content["body_text"][:500])

lib.cwm_stop(h)
lib.cwm_destroy(h)
```

---

## API 说明

### `List(mailbox)`

| 参数 | 说明 |
|------|------|
| `mailbox = ""` | 返回所有邮件目录名（`MailInfo::mailbox` 填目录名，其余为空） |
| `mailbox = "INBOX"` | 返回该目录下所有邮件摘要 |

### `Get(mailbox, uid)`

从服务器实时拉取完整邮件（RFC 822）并解析：
- `body_text` — 纯文本正文
- `body_html` — HTML 正文
- `attachment_names` — 附件文件名列表

### `Del(mailbox, uid)`

- `uid` 非空：发送 `UID STORE +FLAGS (\Deleted)` 再 `EXPUNGE` 删除单封邮件
- `uid` 为空：发送 `DELETE` 命令删除整个目录（**INBOX 不可删除**）

---

## 配置项

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `host` | `imap.exmail.qq.com` | IMAP 服务器 |
| `port` | `993` | SSL 端口 |
| `use_ssl` | `true` | 是否启用 SSL |
| `verify_peer` | `true` | 是否校验服务器证书 |
| `username` | — | 完整邮箱地址 |
| `password` | — | 邮箱密码或授权码 |
| `poll_interval_sec` | `60` | 轮询间隔（0 = 仅手动） |
| `connect_timeout_sec` | `15` | 连接超时 |
| `max_mails_per_poll` | `100` | 每次轮询最多拉取邮件数 |

---

## 微信企业邮箱 IMAP 设置

微信企业邮箱需在邮箱后台开启 IMAP/SMTP 访问权限：

- 服务器：`imap.exmail.qq.com`
- 端口：`993`（SSL）
- 账号：完整邮箱地址（如 `user@company.com`）
- 密码：邮箱登录密码（部分企业需在后台生成专用授权码）
