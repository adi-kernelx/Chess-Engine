#pragma once
#include "auth/email_recovery.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace chess::auth {
// Fixed Gmail transport: no user-controlled SMTP host, sender or headers.
class SmtpMailer {
public:
    SmtpMailer();
    ~SmtpMailer();
    SmtpMailer(const SmtpMailer&)=delete;
    SmtpMailer& operator=(const SmtpMailer&)=delete;
    bool enabled() const { return enabled_; }
    const std::string& public_url() const { return public_url_; }
    EmailSender sender();
private:
    struct Job { std::string to,subject,body; };
    bool enqueue(const std::string&,const std::string&,const std::string&);
    bool deliver(const Job&);
    void run();
    bool enabled_=false, stopping_=false;
    std::string username_,password_,public_url_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> queue_;
    std::thread worker_;
};
} // namespace chess::auth
