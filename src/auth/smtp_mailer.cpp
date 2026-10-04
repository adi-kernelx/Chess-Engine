#include "auth/smtp_mailer.h"
#include "core/logger.h"
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>

namespace chess::auth {
namespace {
std::string setting(const char* name) { const char* value=std::getenv(name); return value?value:""; }
struct Upload { const std::string& data; size_t offset=0; };
size_t read_message(char* out,size_t size,size_t count,void* context) {
    auto& input=*static_cast<Upload*>(context);
    const auto bytes=std::min(size*count,input.data.size()-input.offset);
    std::memcpy(out,input.data.data()+input.offset,bytes); input.offset+=bytes; return bytes;
}
}
SmtpMailer::SmtpMailer() {
    username_=canonical_email(setting("SMTP_USERNAME")); password_=setting("SMTP_PASSWORD");
    public_url_=setting("AUTH_PUBLIC_URL");
    // Only an operator-set origin: prevent header injection and open redirects.
    const std::regex origin(R"(^https://[A-Za-z0-9.-]+(:[0-9]+)?$)");
    if(username_.empty() || password_.empty() || password_.size()>256 || !std::regex_match(public_url_,origin)) return;
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return;
    enabled_=true;
    worker_=std::thread([this]{run();});
}
SmtpMailer::~SmtpMailer() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; queue_.clear(); }
    ready_.notify_all(); if(worker_.joinable()) worker_.join();
    if(!password_.empty()) OPENSSL_cleanse(password_.data(),password_.size());
}
EmailSender SmtpMailer::sender() {
    if(!enabled_) return {};
    return [this](const std::string& to,const std::string& subject,const std::string& body){return enqueue(to,subject,body);};
}
bool SmtpMailer::enqueue(const std::string& to,const std::string& subject,const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_ || queue_.size()>=64) return false;
    queue_.push_back({to,subject,body}); ready_.notify_one(); return true;
}
void SmtpMailer::run() {
    for(;;) {
        Job job;
        { std::unique_lock<std::mutex> lock(mutex_); ready_.wait(lock,[this]{return stopping_ || !queue_.empty();});
          if(stopping_) return;
          job=std::move(queue_.front()); queue_.pop_front(); }
        if(job.body.empty()) continue; // Same enqueue path for unknown accounts.
        if(!deliver(job)) core::Logger::warn("auth","email","Email delivery failed; sensitive diagnostics withheld");
    }
}
bool SmtpMailer::deliver(const Job& job) {
    if(canonical_email(job.to)!=job.to || job.subject.find_first_of("\r\n")!=std::string::npos) return false;
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if(!curl) return false;
    const std::string from="<"+username_+">",to="<"+job.to+">";
    const std::string message="From: Multiplayer Chess "+from+"\r\nTo: "+to+
        "\r\nSubject: "+job.subject+"\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"+job.body;
    Upload upload{message};
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> recipients(curl_slist_append(nullptr,to.c_str()),curl_slist_free_all);
    if(!recipients) return false;
    curl_easy_setopt(curl.get(),CURLOPT_URL,"smtp://smtp.gmail.com:587");
    curl_easy_setopt(curl.get(),CURLOPT_USE_SSL,CURLUSESSL_ALL);
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
    curl_easy_setopt(curl.get(),CURLOPT_USERNAME,username_.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_PASSWORD,password_.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_MAIL_FROM,from.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_MAIL_RCPT,recipients.get());
    curl_easy_setopt(curl.get(),CURLOPT_UPLOAD,1L);
    curl_easy_setopt(curl.get(),CURLOPT_READFUNCTION,read_message);
    curl_easy_setopt(curl.get(),CURLOPT_READDATA,&upload);
    curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,10L);
    curl_easy_setopt(curl.get(),CURLOPT_VERBOSE,0L);
    return curl_easy_perform(curl.get())==CURLE_OK;
}
} // namespace chess::auth
