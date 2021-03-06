#include "AsyncFileAppender.h"
#include <boost/scoped_ptr.hpp>
#include <boost/bind.hpp>
#include "StrUtil.h"
using std::string;
using std::vector;

bool AsyncFileAppender::start()
{
    boost::mutex::scoped_lock guard(runMutex_);
    if(asyncRunning_){
        return true;
    }
    asyncRunning_ = true;    
    thread_.reset(new boost::thread(boost::bind(&AsyncFileAppender::threadFunc,this)));
    if(!thread_){
        asyncRunning_ = false;
        return false;
    }
    return true;
}

void AsyncFileAppender::stop()
{
    boost::mutex::scoped_lock guard(runMutex_);
    if(asyncRunning_){
        asyncRunning_ = false;
        thread_->join();
        thread_.reset();
        return;
    }
}

void AsyncFileAppender::output(char* msg,size_t len)
{
    asyncWriteItem_t* pLogItem = new asyncWriteItem_t(msg,len);
    
    boost::mutex::scoped_lock guard(queueMutex_);
    logQueue_.push_back(pLogItem);
}


void AsyncFileAppender::threadFunc()
{
    asyncWriteItem_t* item = NULL;
    pthread_cleanup_push(AsyncFileAppender::s_dumpQueue,this);
    
    while(asyncRunning_){
        item = NULL;
        vector<asyncWriteItem_t*> tmpQueue;
        logQueue_.reserve(5120000);
        {
            boost::mutex::scoped_lock guard(queueMutex_);
            tmpQueue.swap(logQueue_);
        }
        // if(tmpQueue.empty()){
        //     pthread_timed_wait();
        // }
        // maybe writev??
        size_t size = tmpQueue.size();
        for(size_t i=0;i<size;i++){
            item = tmpQueue[i];
            if(item){
                inThreadOutput(item->msg,item->len);
                delete item;
            }
        }
        usleep(1);
    }

    pthread_cleanup_pop(1);
}

void AsyncFileAppender::inThreadOutput(char* msg,size_t len)
{
    UpdateCurrentTm();
    FileAppender::outputWithoutLock(msg,len);
    ClearCurrentTm();
}

void AsyncFileAppender::s_dumpQueue(void* arg)
{
    AsyncFileAppender* pthis = (AsyncFileAppender*)arg;
    pthis->dumpQueue();
}

void AsyncFileAppender::dumpQueue()
{
    asyncWriteItem_t* item = NULL;
    
    // 退出时，尽量把所有日志写完
    vector<asyncWriteItem_t* > tmpQueue;
    
    {   
        boost::mutex::scoped_lock guard(queueMutex_);
        tmpQueue.swap(logQueue_);
    }
    
    if(tmpQueue.empty()){
        return;
    }
    size_t size = tmpQueue.size();
    for(size_t i=0;i<size;i++){
        item = tmpQueue[i];
        if(item){
            inThreadOutput(item->msg,item->len);
            delete item;
        }
    }
}

// just place-holder function.
void magicCookieFunc()
{}
