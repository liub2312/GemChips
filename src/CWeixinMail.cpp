/**
 * CWeixinMail.cpp
 * 微信企业邮箱自动抓取组件 - 实现
 *
 * 依赖：libcurl（IMAP + SSL）
 * 编译为共享库：
 *   g++ -std=c++17 -fPIC -shared -O2 \
 *       -Iinclude src/CWeixinMail.cpp \
 *       -lcurl -lpthread \
 *       -o libweixinmail.so
 */

#include "CWeixinMail.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

#include <curl/curl.h>

/* ──────────────────────────────────────────────────────────────────────────── *
 *  辅助函数
 * ──────────────────────────────────────────────────────────────────────────── */
namespace {

/** libcurl 写回调：把收到的数据追加到 std::string */
std::size_t curlWriteCb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

/** 简单的 JSON 字符串转义 */
std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

/** 从 IMAP ENVELOPE 响应中提取字段（极简解析） */
std::string extractEnvelopeField(const std::string& envelope, int fieldIndex)
{
    // ENVELOPE 格式：(date subject from sender reply-to to cc bcc in-reply-to message-id)
    // 各字段可能是 NIL、"string" 或嵌套括号
    int idx  = 0;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    std::string cur;
    int fieldCount = -1; // 进入最外层括号后开始计数

    for (std::size_t i = 0; i < envelope.size(); ++i) {
        char c = envelope[i];
        if (escaped) { cur += c; escaped = false; continue; }
        if (c == '\\') { escaped = true; cur += c; continue; }

        if (!inStr && c == '(') {
            ++depth;
            if (depth == 1) { fieldCount = 0; cur.clear(); continue; }
            cur += c;
        } else if (!inStr && c == ')') {
            --depth;
            if (depth == 0) break;
            cur += c;
        } else if (c == '"') {
            inStr = !inStr;
            if (!inStr && fieldCount == fieldIndex) return cur;
            cur.clear();
        } else if (!inStr && c == ' ' && depth == 1) {
            if (fieldCount == fieldIndex && cur != "NIL") return cur;
            ++fieldCount;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (fieldCount == fieldIndex && !cur.empty() && cur != "NIL") return cur;
    return "";
}

/** 从地址结构中提取可读地址，如 "user@host.com" */
std::string parseAddress(const std::string& addrStruct)
{
    // 取 name mailbox host 三部分
    auto getStr = [&](const std::string& s, int idx) -> std::string {
        return extractEnvelopeField(s, idx);
    };
    std::string mailbox = getStr(addrStruct, 2);
    std::string host    = getStr(addrStruct, 3);
    if (!mailbox.empty() && !host.empty())
        return mailbox + "@" + host;
    return "";
}

/** 从 FETCH ENVELOPE 行解析 MailInfo */
weixin::MailInfo parseEnvelope(const std::string& line, const std::string& mailbox)
{
    weixin::MailInfo info;
    info.mailbox = mailbox;

    // 提取 UID（行首：<uid> FETCH (UID <n> ...）
    {
        std::regex re_uid(R"(UID\s+(\d+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re_uid)) info.uid = m[1];
    }
    // ENVELOPE 子串
    {
        std::regex re_env(R"(ENVELOPE\s*(\(.*\)))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re_env)) {
            std::string env = m[1];
            info.date    = extractEnvelopeField(env, 0);
            info.subject = extractEnvelopeField(env, 1);
            // from 字段是 ((name adl mailbox host))
            std::string from_raw = extractEnvelopeField(env, 2);
            info.from = parseAddress(from_raw);
        }
    }
    // FLAGS
    {
        std::regex re_flags(R"(FLAGS\s*\(([^)]*)\))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re_flags))
            info.seen = (m[1].str().find("\\Seen") != std::string::npos);
    }
    return info;
}

/** 从 RFC822 原始邮件中提取文本正文（极简解析） */
void parseRaw822(const std::string& raw,
                 std::string& out_header,
                 std::string& out_text,
                 std::string& out_html,
                 std::vector<std::string>& out_attachments)
{
    // 分割头部与主体
    auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) sep = raw.find("\n\n");
    if (sep == std::string::npos) { out_header = raw; return; }

    out_header  = raw.substr(0, sep);
    std::string body = raw.substr(sep + 4);

    // 简单检测 Content-Type boundary
    std::regex re_ct(R"(Content-Type:\s*([^\r\n;]+))", std::regex::icase);
    std::smatch mct;
    std::string ctype;
    if (std::regex_search(out_header, mct, re_ct)) ctype = mct[1];

    if (ctype.find("multipart") != std::string::npos) {
        // 提取 boundary
        std::regex re_b(R"rx(boundary="?([^"\r\n;]+)"?)rx", std::regex::icase);
        std::smatch mb;
        std::string boundary;
        if (std::regex_search(out_header, mb, re_b)) boundary = mb[1];

        if (!boundary.empty()) {
            std::string delim = "--" + boundary;
            std::size_t pos = 0;
            while ((pos = body.find(delim, pos)) != std::string::npos) {
                pos += delim.size();
                if (pos < body.size() && body[pos] == '-') break; // last boundary
                auto end = body.find(delim, pos);
                std::string part = (end != std::string::npos)
                                   ? body.substr(pos, end - pos)
                                   : body.substr(pos);

                // 分割 part 头和内容
                auto psep = part.find("\r\n\r\n");
                if (psep == std::string::npos) psep = part.find("\n\n");
                if (psep == std::string::npos) continue;
                std::string ph = part.substr(0, psep);
                std::string pb = part.substr(psep + 4);

                std::regex re_pct(R"(Content-Type:\s*([^\r\n;]+))", std::regex::icase);
                std::smatch mpc;
                if (!std::regex_search(ph, mpc, re_pct)) continue;
                std::string pct = mpc[1];

                // Content-Disposition: attachment
                if (ph.find("attachment") != std::string::npos) {
                    std::regex re_fn(R"rx(filename="?([^"\r\n;]+)"?)rx", std::regex::icase);
                    std::smatch mfn;
                    if (std::regex_search(ph, mfn, re_fn))
                        out_attachments.push_back(mfn[1]);
                    continue;
                }
                if (pct.find("text/plain") != std::string::npos && out_text.empty())
                    out_text = pb;
                else if (pct.find("text/html") != std::string::npos && out_html.empty())
                    out_html = pb;
            }
        }
    } else if (ctype.find("text/html") != std::string::npos) {
        out_html = body;
    } else {
        out_text = body;
    }
}

} // anonymous namespace

/* ──────────────────────────────────────────────────────────────────────────── *
 *  CWeixinMail::Impl
 * ──────────────────────────────────────────────────────────────────────────── */
namespace weixin {

struct CWeixinMail::Impl {
    MailConfig  cfg;
    std::atomic<bool> running{false};
    std::thread       poll_thread;
    std::mutex        cv_mutex;
    std::condition_variable cv;

    // 缓存：mailbox -> [MailInfo]
    std::mutex                              cache_mutex;
    std::map<std::string, std::vector<MailInfo>> mail_cache;
    // 已知目录
    std::vector<std::string>               mailboxes;
    // seen UID 集合（避免重复通知）
    std::map<std::string, std::set<std::string>> seen_uids;

    std::string last_error;
    NewMailCallback new_mail_cb;

    /* ── curl 辅助 ── */

    /** 构造 IMAPS URL */
    std::string makeUrl(const std::string& mailbox = "", const std::string& cmd = "") const
    {
        std::ostringstream oss;
        oss << (cfg.use_ssl ? "imaps://" : "imap://")
            << cfg.host << ":" << cfg.port << "/";
        if (!mailbox.empty()) {
            // URL-encode 空格为 %20（简单处理）
            for (char c : mailbox)
                oss << (c == ' ' ? "%20" : std::string(1, c));
        }
        if (!cmd.empty()) oss << "?" << cmd;
        return oss.str();
    }

    /** 设置通用 curl 选项 */
    void setupCurl(CURL* curl, const std::string& url, std::string& buf) const
    {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
        curl_easy_setopt(curl, CURLOPT_USE_SSL,  cfg.use_ssl ? CURLUSESSL_ALL : CURLUSESSL_NONE);
        if (!cfg.verify_peer) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)cfg.connect_timeout_sec);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    }

    /** 执行 curl 请求，返回 CURLcode */
    CURLcode perform(const std::string& url, std::string& buf)
    {
        CURL* curl = curl_easy_init();
        if (!curl) { last_error = "curl_easy_init failed"; return CURLE_FAILED_INIT; }
        setupCurl(curl, url, buf);
        CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) last_error = curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        return rc;
    }

    /** 执行带自定义命令（CURLOPT_CUSTOMREQUEST）的请求 */
    CURLcode performCmd(const std::string& url, const std::string& customCmd, std::string& buf)
    {
        CURL* curl = curl_easy_init();
        if (!curl) { last_error = "curl_easy_init failed"; return CURLE_FAILED_INIT; }
        setupCurl(curl, url, buf);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, customCmd.c_str());
        CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) last_error = curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        return rc;
    }

    /* ── 功能方法 ── */

    bool fetchMailboxList()
    {
        std::string buf;
        auto rc = perform(makeUrl(), buf);
        if (rc != CURLE_OK) return false;

        // 解析 LIST 响应，每行形如：* LIST (\HasNoChildren) "/" "INBOX"
        std::regex re(R"rx(\* LIST[^\r\n]+"([^"]+)")rx", std::regex::icase);
        std::sregex_iterator it(buf.begin(), buf.end(), re), end;
        std::lock_guard<std::mutex> lk(cache_mutex);
        mailboxes.clear();
        for (; it != end; ++it) {
            std::string mb = (*it)[1];
            mailboxes.push_back(mb);
            if (mail_cache.find(mb) == mail_cache.end())
                mail_cache[mb] = {};
        }
        // 若解析不到带引号的，再试无引号格式
        if (mailboxes.empty()) {
            std::regex re2(R"(\* LIST[^\r\n]+\s(\S+)\s*$)");
            std::sregex_iterator it2(buf.begin(), buf.end(), re2), end2;
            for (; it2 != end2; ++it2) {
                std::string mb = (*it2)[1];
                if (mb.front() == '"') mb = mb.substr(1, mb.size()-2);
                mailboxes.push_back(mb);
            }
        }
        return true;
    }

    /** 拉取指定 mailbox 的邮件列表（UID + ENVELOPE + FLAGS） */
    bool fetchMailbox(const std::string& mailbox)
    {
        // IMAP SEARCH ALL，由 libcurl 通过 CURLOPT_CUSTOMREQUEST 实现
        // libcurl IMAP 的 URL 格式：imaps://host/MAILBOX?SEARCH ALL
        std::string buf;
        // Step 1: SEARCH ALL -> 获取消息序号列表
        auto searchUrl = makeUrl(mailbox, "ALL");
        auto rc = perform(searchUrl, buf);
        if (rc != CURLE_OK) return false;

        // buf 包含序号，格式：* SEARCH 1 2 3 ...
        std::vector<std::string> seq_nums;
        {
            std::regex re(R"(\* SEARCH([\d\s]*))", std::regex::icase);
            std::smatch m;
            if (std::regex_search(buf, m, re)) {
                std::istringstream iss(m[1]);
                std::string n;
                int count = 0;
                while (iss >> n && count < cfg.max_mails_per_poll) {
                    seq_nums.push_back(n);
                    ++count;
                }
            }
        }
        if (seq_nums.empty()) {
            // 清空缓存
            std::lock_guard<std::mutex> lk(cache_mutex);
            mail_cache[mailbox].clear();
            return true;
        }

        // Step 2: FETCH UID ENVELOPE FLAGS
        // libcurl IMAP 自定义命令：UID FETCH 1:* (UID FLAGS ENVELOPE)
        std::string fetchCmd = "UID FETCH 1:* (UID FLAGS ENVELOPE)";
        std::string fetchUrl = makeUrl(mailbox);
        std::string fetchBuf;
        rc = performCmd(fetchUrl, fetchCmd, fetchBuf);
        if (rc != CURLE_OK) return false;

        // 解析响应：每条 FETCH 可能跨多行，简单按 "* \d+ FETCH" 分割
        std::vector<MailInfo> mails;
        std::regex re_fetch(R"(\*\s+\d+\s+FETCH\s+\()");
        auto it = std::sregex_iterator(fetchBuf.begin(), fetchBuf.end(), re_fetch);
        auto itEnd = std::sregex_iterator();
        std::vector<std::size_t> positions;
        for (; it != itEnd; ++it) positions.push_back(it->position());

        for (std::size_t i = 0; i < positions.size(); ++i) {
            std::size_t start = positions[i];
            std::size_t length = (i + 1 < positions.size())
                                 ? positions[i+1] - start
                                 : fetchBuf.size() - start;
            std::string segment = fetchBuf.substr(start, length);
            MailInfo info = parseEnvelope(segment, mailbox);
            if (!info.uid.empty()) mails.push_back(std::move(info));
        }

        // 通知新邮件
        if (new_mail_cb) {
            std::vector<MailInfo> newMails;
            auto& knownSet = seen_uids[mailbox];
            for (auto& m : mails) {
                if (knownSet.find(m.uid) == knownSet.end()) {
                    newMails.push_back(m);
                    knownSet.insert(m.uid);
                }
            }
            if (!newMails.empty()) new_mail_cb(newMails);
        }

        std::lock_guard<std::mutex> lk(cache_mutex);
        mail_cache[mailbox] = std::move(mails);
        return true;
    }

    /** 拉取单封邮件原始内容 */
    bool fetchMailContent(const std::string& mailbox, const std::string& uid,
                          std::string& raw822)
    {
        // UID FETCH <uid> (RFC822)
        std::string cmd = "UID FETCH " + uid + " (RFC822)";
        std::string buf;
        auto rc = performCmd(makeUrl(mailbox), cmd, buf);
        if (rc != CURLE_OK) return false;

        // 去掉 FETCH 响应头，提取实际 RFC822 内容
        auto pos = buf.find("\r\n");
        if (pos == std::string::npos) pos = buf.find("\n");
        if (pos != std::string::npos) {
            // 跳过第一行（* N FETCH (RFC822 {size}）和末尾 ")"
            auto start = buf.find("\r\n", pos);
            if (start == std::string::npos) start = buf.find("\n", pos);
            if (start != std::string::npos) {
                raw822 = buf.substr(start + 2);
                // 去尾部的 ")\r\nN OK ..."
                auto tail = raw822.rfind("\r\n)\r\n");
                if (tail != std::string::npos) raw822 = raw822.substr(0, tail);
            } else {
                raw822 = buf;
            }
        } else {
            raw822 = buf;
        }
        return true;
    }

    /** 删除邮件（IMAP STORE + EXPUNGE） */
    bool deleteMail(const std::string& mailbox, const std::string& uid)
    {
        // UID STORE <uid> +FLAGS (\Deleted)
        {
            std::string cmd = "UID STORE " + uid + " +FLAGS (\\Deleted)";
            std::string buf;
            performCmd(makeUrl(mailbox), cmd, buf);
        }
        // EXPUNGE
        {
            std::string cmd = "EXPUNGE";
            std::string buf;
            performCmd(makeUrl(mailbox), cmd, buf);
        }
        // 从缓存移除
        std::lock_guard<std::mutex> lk(cache_mutex);
        auto it = mail_cache.find(mailbox);
        if (it != mail_cache.end()) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(),
                [&uid](const MailInfo& m){ return m.uid == uid; }), v.end());
        }
        return true;
    }

    /** 删除整个 mailbox（不含 INBOX） */
    bool deleteMailbox(const std::string& mailbox)
    {
        if (mailbox == "INBOX") { last_error = "Cannot delete INBOX"; return false; }
        std::string cmd = "DELETE \"" + mailbox + "\"";
        std::string buf;
        auto rc = performCmd(makeUrl(), cmd, buf);
        if (rc != CURLE_OK) return false;
        std::lock_guard<std::mutex> lk(cache_mutex);
        mail_cache.erase(mailbox);
        mailboxes.erase(std::remove(mailboxes.begin(), mailboxes.end(), mailbox), mailboxes.end());
        return true;
    }

    /** 一次完整轮询 */
    void doPoll()
    {
        fetchMailboxList();
        std::vector<std::string> mbs;
        {
            std::lock_guard<std::mutex> lk(cache_mutex);
            mbs = mailboxes;
        }
        for (auto& mb : mbs) fetchMailbox(mb);
    }

    /** 后台线程入口 */
    void threadFunc()
    {
        while (running.load()) {
            doPoll();
            if (cfg.poll_interval_sec <= 0) break;
            std::unique_lock<std::mutex> lk(cv_mutex);
            cv.wait_for(lk, std::chrono::seconds(cfg.poll_interval_sec),
                        [this]{ return !running.load(); });
        }
    }
};

/* ──────────────────────────────────────────────────────────────────────────── *
 *  CWeixinMail 公开接口实现
 * ──────────────────────────────────────────────────────────────────────────── */

CWeixinMail::CWeixinMail() : impl_(std::make_unique<Impl>()) {}

CWeixinMail::~CWeixinMail() { Stop(); }

bool CWeixinMail::Init(const MailConfig& config)
{
    impl_->cfg = config;
    // 验证必填项
    if (config.username.empty() || config.password.empty()) {
        impl_->last_error = "username and password are required";
        return false;
    }
    // 初始化 libcurl（全局，幂等）
    curl_global_init(CURL_GLOBAL_DEFAULT);
    // 测试连接：拉取目录列表
    return impl_->fetchMailboxList();
}

void CWeixinMail::Start()
{
    if (impl_->running.exchange(true)) return; // 已在运行
    impl_->poll_thread = std::thread([this]{ impl_->threadFunc(); });
}

void CWeixinMail::Stop()
{
    if (!impl_->running.exchange(false)) return;
    impl_->cv.notify_all();
    if (impl_->poll_thread.joinable()) impl_->poll_thread.join();
}

bool CWeixinMail::IsRunning() const { return impl_->running.load(); }

void CWeixinMail::PollNow()
{
    // 在调用线程中同步执行一次轮询
    impl_->doPoll();
    // 若后台线程在等待，重置计时器
    impl_->cv.notify_all();
}

std::vector<MailInfo> CWeixinMail::List(const std::string& mailbox)
{
    std::lock_guard<std::mutex> lk(impl_->cache_mutex);
    if (mailbox.empty()) {
        // 返回目录列表：每个目录作为一个 MailInfo，mailbox 字段填目录名
        std::vector<MailInfo> result;
        result.reserve(impl_->mailboxes.size());
        for (auto& mb : impl_->mailboxes) {
            MailInfo info;
            info.mailbox = mb;
            result.push_back(std::move(info));
        }
        return result;
    }
    auto it = impl_->mail_cache.find(mailbox);
    if (it != impl_->mail_cache.end()) return it->second;
    return {};
}

MailContent CWeixinMail::Get(const std::string& mailbox, const std::string& uid)
{
    MailContent content;
    // 先从缓存找摘要
    {
        std::lock_guard<std::mutex> lk(impl_->cache_mutex);
        auto it = impl_->mail_cache.find(mailbox);
        if (it != impl_->mail_cache.end()) {
            for (auto& m : it->second) {
                if (m.uid == uid) { content.info = m; break; }
            }
        }
    }
    if (content.info.uid.empty()) {
        impl_->last_error = "uid " + uid + " not found in " + mailbox;
        return content;
    }
    // 拉取完整内容
    std::string raw822;
    if (!impl_->fetchMailContent(mailbox, uid, raw822)) return content;
    parseRaw822(raw822, content.raw_header, content.body_text,
                content.body_html, content.attachment_names);
    return content;
}

bool CWeixinMail::Del(const std::string& mailbox, const std::string& uid)
{
    if (uid.empty()) return impl_->deleteMailbox(mailbox);
    return impl_->deleteMail(mailbox, uid);
}

void CWeixinMail::SetNewMailCallback(NewMailCallback cb) { impl_->new_mail_cb = std::move(cb); }

std::string CWeixinMail::LastError() const { return impl_->last_error; }

} // namespace weixin

/* ──────────────────────────────────────────────────────────────────────────── *
 *  C-ABI 导出实现
 * ──────────────────────────────────────────────────────────────────────────── */

struct CWMHandle {
    weixin::CWeixinMail obj;
    std::string last_error_buf;
};

extern "C" {

void* cwm_create(void)
{
    return new CWMHandle();
}

void cwm_destroy(void* handle)
{
    delete static_cast<CWMHandle*>(handle);
}

int cwm_init(void* handle,
             const char* host,
             int         port,
             const char* username,
             const char* password,
             int         poll_interval)
{
    auto* h = static_cast<CWMHandle*>(handle);
    weixin::MailConfig cfg;
    if (host && *host) cfg.host = host;
    if (port > 0)      cfg.port = port;
    cfg.username          = username ? username : "";
    cfg.password          = password ? password : "";
    cfg.poll_interval_sec = poll_interval;
    return h->obj.Init(cfg) ? 1 : 0;
}

void cwm_start(void* handle)
{
    static_cast<CWMHandle*>(handle)->obj.Start();
}

void cwm_stop(void* handle)
{
    static_cast<CWMHandle*>(handle)->obj.Stop();
}

void cwm_poll_now(void* handle)
{
    static_cast<CWMHandle*>(handle)->obj.PollNow();
}

static char* dupstr(const std::string& s)
{
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

char* cwm_list(void* handle, const char* mailbox)
{
    auto* h = static_cast<CWMHandle*>(handle);
    std::string mb = (mailbox && *mailbox) ? mailbox : "";
    auto items = h->obj.List(mb);

    std::ostringstream oss;
    oss << "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) oss << ",";
        if (mb.empty()) {
            // 目录列表
            oss << "\"" << jsonEscape(items[i].mailbox) << "\"";
        } else {
            oss << "{"
                << "\"uid\":\"" << jsonEscape(items[i].uid) << "\","
                << "\"subject\":\"" << jsonEscape(items[i].subject) << "\","
                << "\"from\":\"" << jsonEscape(items[i].from) << "\","
                << "\"date\":\"" << jsonEscape(items[i].date) << "\","
                << "\"seen\":" << (items[i].seen ? "true" : "false")
                << "}";
        }
    }
    oss << "]";
    return dupstr(oss.str());
}

char* cwm_get(void* handle, const char* mailbox, const char* uid)
{
    auto* h = static_cast<CWMHandle*>(handle);
    std::string mb  = mailbox ? mailbox : "";
    std::string uid_= uid     ? uid     : "";
    auto c = h->obj.Get(mb, uid_);

    std::ostringstream oss;
    if (c.info.uid.empty()) {
        oss << "{}";
    } else {
        oss << "{"
            << "\"uid\":\"" << jsonEscape(c.info.uid) << "\","
            << "\"mailbox\":\"" << jsonEscape(c.info.mailbox) << "\","
            << "\"subject\":\"" << jsonEscape(c.info.subject) << "\","
            << "\"from\":\"" << jsonEscape(c.info.from) << "\","
            << "\"date\":\"" << jsonEscape(c.info.date) << "\","
            << "\"seen\":" << (c.info.seen ? "true" : "false") << ","
            << "\"body_text\":\"" << jsonEscape(c.body_text) << "\","
            << "\"body_html\":\"" << jsonEscape(c.body_html) << "\","
            << "\"attachments\":[";
        for (std::size_t i = 0; i < c.attachment_names.size(); ++i) {
            if (i) oss << ",";
            oss << "\"" << jsonEscape(c.attachment_names[i]) << "\"";
        }
        oss << "]}";
    }
    return dupstr(oss.str());
}

int cwm_del(void* handle, const char* mailbox, const char* uid)
{
    auto* h = static_cast<CWMHandle*>(handle);
    std::string mb  = mailbox ? mailbox : "";
    std::string uid_= (uid && *uid) ? uid : "";
    return h->obj.Del(mb, uid_) ? 1 : 0;
}

const char* cwm_last_error(void* handle)
{
    auto* h = static_cast<CWMHandle*>(handle);
    h->last_error_buf = h->obj.LastError();
    return h->last_error_buf.c_str();
}

} // extern "C"
