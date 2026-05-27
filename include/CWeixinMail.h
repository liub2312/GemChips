#pragma once

/**
 * CWeixinMail - 微信企业邮箱自动抓取组件
 *
 * 通过 IMAP over SSL 协议连接微信企业邮箱（imap.exmail.qq.com:993），
 * 后台线程按设定间隔自动拉取并缓存邮件；对外提供 List / Get / Del 接口。
 * 编译为共享库（.so）后可由任意语言通过 C-ABI 加载调用。
 *
 * 依赖：libcurl（需带 IMAP + SSL 支持）
 */

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace weixin {

/* ──────────────────────────────────────────────────────────────────────────── *
 *  数据结构
 * ──────────────────────────────────────────────────────────────────────────── */

/** 邮件摘要（List 返回） */
struct MailInfo {
    std::string uid;       ///< IMAP UID（同一 mailbox 内唯一）
    std::string mailbox;   ///< 所在邮箱目录，如 "INBOX"
    std::string subject;   ///< 主题
    std::string from;      ///< 发件人
    std::string date;      ///< 日期字符串
    bool        seen{false};///< 是否已读
};

/** 邮件完整内容（Get 返回） */
struct MailContent {
    MailInfo              info;        ///< 摘要信息
    std::string           raw_header;  ///< 原始邮件头
    std::string           body_text;   ///< 纯文本正文（可能为空）
    std::string           body_html;   ///< HTML 正文（可能为空）
    std::vector<std::string> attachment_names; ///< 附件文件名列表
};

/** 初始化配置 */
struct MailConfig {
    std::string host{"imap.exmail.qq.com"}; ///< IMAP 服务器
    int         port{993};                  ///< IMAP over SSL 端口
    bool        use_ssl{true};              ///< 是否启用 SSL/TLS
    bool        verify_peer{true};          ///< 是否校验服务器证书
    std::string username;                   ///< 完整邮箱地址，如 user@example.com
    std::string password;                   ///< 邮箱密码 / 授权码
    int         poll_interval_sec{60};      ///< 自动轮询间隔（秒），0 表示仅手动
    int         connect_timeout_sec{15};    ///< 连接超时（秒）
    int         max_mails_per_poll{100};    ///< 每次轮询最多拉取邮件数
};

/* ──────────────────────────────────────────────────────────────────────────── *
 *  主类
 * ──────────────────────────────────────────────────────────────────────────── */

class CWeixinMail {
public:
    CWeixinMail();
    ~CWeixinMail();

    // 禁止拷贝
    CWeixinMail(const CWeixinMail&)            = delete;
    CWeixinMail& operator=(const CWeixinMail&) = delete;

    /**
     * 初始化并连接邮箱
     * @return true 成功；false 失败（通过 LastError() 获取错误信息）
     */
    bool Init(const MailConfig& config);

    /**
     * 启动后台自动轮询线程
     * 若 poll_interval_sec == 0 则不启动自动轮询
     */
    void Start();

    /** 停止后台轮询线程并释放资源 */
    void Stop();

    /** 是否已在运行 */
    bool IsRunning() const;

    /**
     * 立即触发一次手动轮询（线程安全）
     */
    void PollNow();

    /**
     * 列出指定目录下的所有邮件摘要，若 mailbox 为空则列出所有目录名
     *
     * @param mailbox  IMAP 目录名，如 "INBOX"、"Sent Messages" 等
     *                 传空字符串 "" 时返回目录列表（MailInfo::mailbox 填目录名，其余字段为空）
     * @return 邮件摘要列表（从缓存读取，不阻塞）
     */
    std::vector<MailInfo> List(const std::string& mailbox = "INBOX");

    /**
     * 获取指定邮件的完整内容
     *
     * @param mailbox  目录名
     * @param uid      邮件 UID
     * @return 邮件内容；若未找到则 info.uid 为空
     */
    MailContent Get(const std::string& mailbox, const std::string& uid);

    /**
     * 删除邮件或整个目录
     *
     * @param mailbox  目录名
     * @param uid      邮件 UID；若为空则删除整个目录（谨慎使用）
     * @return true 成功
     */
    bool Del(const std::string& mailbox, const std::string& uid = "");

    /**
     * 注册新邮件到达回调（在轮询线程中调用）
     * @param cb  回调函数，参数为新邮件摘要列表
     */
    using NewMailCallback = std::function<void(const std::vector<MailInfo>&)>;
    void SetNewMailCallback(NewMailCallback cb);

    /** 返回最近一次错误描述 */
    std::string LastError() const;

private:
    /* ── 内部实现 ── */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace weixin

/* ──────────────────────────────────────────────────────────────────────────── *
 *  C-ABI 导出（供 dlopen / FFI 调用）
 * ──────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/** 创建实例 */
void* cwm_create(void);

/** 销毁实例 */
void  cwm_destroy(void* handle);

/**
 * 初始化
 * @param handle         实例指针
 * @param host           IMAP 服务器（NULL 使用默认 imap.exmail.qq.com）
 * @param port           端口（0 使用默认 993）
 * @param username       用户名（邮箱地址）
 * @param password       密码
 * @param poll_interval  轮询间隔秒数（0 不自动轮询）
 * @return 1 成功，0 失败
 */
int   cwm_init(void* handle,
               const char* host,
               int         port,
               const char* username,
               const char* password,
               int         poll_interval);

/** 启动后台轮询 */
void  cwm_start(void* handle);

/** 停止后台轮询 */
void  cwm_stop(void* handle);

/** 立即轮询一次 */
void  cwm_poll_now(void* handle);

/**
 * 列出目录或邮件（JSON 字符串，调用者需 free()）
 *
 * mailbox 为 NULL 或 "" 时返回目录列表 JSON；
 * 否则返回指定目录的邮件摘要 JSON 数组。
 * JSON 格式：
 *   目录列表：["INBOX","Sent Messages",...]
 *   邮件列表：[{"uid":"...","subject":"...","from":"...","date":"...","seen":true/false},...]
 */
char* cwm_list(void* handle, const char* mailbox);

/**
 * 获取邮件内容（JSON 字符串，调用者需 free()）
 * JSON 格式：{"uid":"...","subject":"...","from":"...","date":"...","body_text":"...","body_html":"...","attachments":["file1.pdf",...]}
 */
char* cwm_get(void* handle, const char* mailbox, const char* uid);

/**
 * 删除邮件或目录
 * uid 为 NULL 或 "" 时删除整个 mailbox
 * @return 1 成功，0 失败
 */
int   cwm_del(void* handle, const char* mailbox, const char* uid);

/** 返回最近错误（不需要 free，生命周期与 handle 一致） */
const char* cwm_last_error(void* handle);

#ifdef __cplusplus
} // extern "C"
#endif
