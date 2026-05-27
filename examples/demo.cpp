/**
 * demo.cpp - CWeixinMail 使用示例
 *
 * 编译（与 .so 链接）：
 *   g++ -std=c++17 -Iinclude examples/demo.cpp -L. -lweixinmail -Wl,-rpath,. -o demo
 * 或直接编译所有源文件：
 *   g++ -std=c++17 -Iinclude examples/demo.cpp src/CWeixinMail.cpp -lcurl -lpthread -o demo
 */

#include <iostream>
#include <thread>
#include <chrono>
#include "CWeixinMail.h"

int main()
{
    weixin::CWeixinMail mail;

    weixin::MailConfig cfg;
    cfg.host              = "imap.exmail.qq.com";
    cfg.port              = 993;
    cfg.use_ssl           = true;
    cfg.verify_peer       = true;
    cfg.username          = "your_email@example.com";   // ← 修改为实际账号
    cfg.password          = "your_password_or_token";   // ← 修改为实际密码
    cfg.poll_interval_sec = 30;   // 每 30 秒自动拉取一次
    cfg.max_mails_per_poll = 50;

    // 注册新邮件回调
    mail.SetNewMailCallback([](const std::vector<weixin::MailInfo>& newMails) {
        std::cout << "[New Mail] " << newMails.size() << " new mail(s):\n";
        for (auto& m : newMails)
            std::cout << "  [" << m.uid << "] " << m.subject << " from " << m.from << "\n";
    });

    std::cout << "Initializing...\n";
    if (!mail.Init(cfg)) {
        std::cerr << "Init failed: " << mail.LastError() << "\n";
        return 1;
    }
    std::cout << "Init OK\n";

    // 列出所有目录
    {
        auto dirs = mail.List("");
        std::cout << "Mailboxes (" << dirs.size() << "):\n";
        for (auto& d : dirs) std::cout << "  " << d.mailbox << "\n";
    }

    // 列出 INBOX 中的邮件
    {
        auto mails = mail.List("INBOX");
        std::cout << "INBOX has " << mails.size() << " mail(s):\n";
        for (auto& m : mails) {
            std::cout << "  UID=" << m.uid
                      << " [" << (m.seen ? "READ" : "UNREAD") << "]"
                      << " " << m.subject
                      << " <" << m.from << ">"
                      << " " << m.date << "\n";
        }

        // 读取第一封邮件的完整内容
        if (!mails.empty()) {
            std::cout << "\nFetching first mail UID=" << mails[0].uid << " ...\n";
            auto content = mail.Get("INBOX", mails[0].uid);
            if (!content.info.uid.empty()) {
                std::cout << "Subject : " << content.info.subject << "\n";
                std::cout << "From    : " << content.info.from << "\n";
                std::cout << "Date    : " << content.info.date << "\n";
                std::cout << "--- Body (text, first 500 chars) ---\n";
                auto text = content.body_text.empty() ? content.body_html : content.body_text;
                std::cout << text.substr(0, 500) << "\n";
                if (!content.attachment_names.empty()) {
                    std::cout << "Attachments:\n";
                    for (auto& a : content.attachment_names)
                        std::cout << "  " << a << "\n";
                }
            } else {
                std::cerr << "Get failed: " << mail.LastError() << "\n";
            }
        }
    }

    // 启动后台自动轮询
    std::cout << "\nStarting background poll (interval=" << cfg.poll_interval_sec << "s)...\n";
    mail.Start();

    // 运行 2 分钟后退出（实际服务中应改为信号等待）
    std::cout << "Running for 120 seconds. Press Ctrl+C to quit early.\n";
    std::this_thread::sleep_for(std::chrono::seconds(120));

    mail.Stop();
    std::cout << "Done.\n";
    return 0;
}
