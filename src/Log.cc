#include <sys/stat.h>
#include "Log.h"
#include "Conf.h"
#include "StrUtil.h"
using namespace std;


CLogger::CLogger(const string& logname,Appender* pAppender,const string& fmt,int level)
    :logname_(NULL),formatter_(fmt),level_(level),pAppender_(pAppender)
{
    logname_ = strdup(logname.c_str());
    assert(logname_);
}

CLogger::~CLogger()
{
    if(pAppender_){
        pAppender_->stop();
    }
    if(logname_){
        free(logname_);
        logname_ = NULL;
    }
}

vector<string> CLogger::splitCheck(const string& instance)
{
    vector<string> ret = split(instance,',',5);
    if(ret.size()!=5){
        return vector<string>();
    }
    trim(ret[0]);
    trim(ret[1]);    
    trim(ret[2]);
    trim(ret[3]);
    trim(ret[4]);
    if(ret[0].empty() || ret[1].empty() || ret[2].empty()){
        return vector<string>();
    }
    if(ret[4].empty() || atoi(ret[4].c_str()) < 0 || atoi(ret[4].c_str()) > LEVEL_ALL){
        char buf[20];
        sprintf(buf,"%d",LEVEL_ALL);
        ret[4]=buf;
    }
    return ret;
}

int32_t CLogger::init(const string& path)
{
    // level = NONE,nothing can log.
    dummyLogger = new CLogger("",NULL,"",NONE);
    assert(dummyLogger);
    
    if(unlikely(!logMap_.empty())){
        fprintf(stderr,"Logger is already inited!!!\n");
        return -1;
    }
    int fd=::open(path.c_str(),O_RDONLY);
    if(fd < 0){
        fprintf(stderr,"Log init fail.file %s not exist!\n",path.c_str());
        return -1;
    }

    TextConfig conf(path,":","#");
    string logpath = conf.getString("log.prefix");
    if(logpath.empty()){
        logpath="./";           // default current dir.
    }

    bool async = false;
    string asyncLogging = conf.getString("log.async");
    if(asyncLogging == "true"){
        async = true;
        fprintf(stderr,"Use Async Logging!\n");
    }

    string splitFormat = conf.getString("log.splitFormat");
    if(splitFormat.empty()){
        splitFormat = "_%Y%m%d_%H%M%S_%P_%n";
    }
    
    vector<string> vec = conf.getStrings("log.instance");
    for(vector<string>::iterator it=vec.begin();it!=vec.end();++it){
        if(it->empty()){
            fprintf(stderr,"You have empty log.instance, ingored.\n");
            continue;
        }        
        
        vector<string> logInstance = splitCheck(*it);
        // splitCheck will asure we got valid config in each exist field.
        // log.instance config format
        // log.instance:instanceName,format,filename,rollsize,level
        // log.instance:xmpp,%L %T %P %f(%l):%F %m,xmpp.log,20000000,7
        if(logInstance.size()>=5){
            if(logMap_.find(logInstance[0]) != logMap_.end()){
                fprintf(stderr,"duplicated log.instance found.config=%s",it->c_str());
                exit(1);
            }
            string realPath = toRealPath(logInstance[2],logpath,logInstance[0]+".log");
            assert(!realPath.empty());
            size_t splitSize = static_cast<size_t>(strtoul(logInstance[3].c_str(),NULL,10));
            Appender* appender = NULL;
            map<string,Appender*>::iterator mapit = appenderMap_.find(realPath);
            if(mapit != appenderMap_.end()){
                appender = mapit->second;
            }
            if(appender == NULL){
                if(async){
                    appender = new AsyncFileAppender(realPath,splitSize,splitFormat);
                }
                else{
                    appender = new FileAppender(realPath,splitSize,splitFormat);
                }
                
                if(!appender->start()){
                    fprintf(stderr,"appender start failed!path=%s\n",realPath.c_str());
                }
                appenderMap_.insert(make_pair(realPath,appender));
            }
            logMap_.insert(make_pair(logInstance[0],new CLogger(logInstance[0],appender,logInstance[1],atoi(logInstance[4].c_str()))));
            fprintf(stderr,"one instance configged.str=%s\n",it->c_str());
        }
        else{
            fprintf(stderr,"You have wrong log.instance config format. Please check. config=%s\n",it->c_str());
            exit(1);
        }
        
    }

    logMapIter wfIt = logMap_.find(WFLOGINSTANCE);
    if(wfIt != logMap_.end()){
        setWfLogInstance(wfIt->second);
    }
    else{
        setWfLogInstance(dummyLogger);
    }
    
    ::atexit(CLogger::destroy);
    return 0;
}

void CLogger::destroy()
{
    boost::mutex::scoped_lock guard(mutex_);
    for (logMapIter it = logMap_.begin(); it != logMap_.end(); ++it){
        delete it->second;
    }
    logMap_.clear();

    for(map<string,Appender*>::iterator it=appenderMap_.begin();it!=appenderMap_.end();++it){
        delete it->second;
    }
    appenderMap_.clear();
}

CLogger& CLogger::GetInstance(const string& logname)
{
    boost::mutex::scoped_lock guard(mutex_);
    logMapIter it = logMap_.find(logname);
    if(it != logMap_.end()){
        return *it->second;
    }
    return *dummyLogger;
}

void CLogger::writeLog(const char* file,int line,const char* func,int level,const char* fmt,va_list args)
{
    char* msg = (char*)malloc(MAX_SIZE_PER_LOG);
    if(!msg) return;

    UpdateCurrentTm();
    
    size_t msg_len=0;
    msg_len = formatter_.format(msg,MAX_SIZE_PER_LOG,logname_,file,line,func,level,fmt,args);
    if(msg_len >= MAX_SIZE_PER_LOG){ // see format or snprintf().
        msg_len = MAX_SIZE_PER_LOG-1; // '\n' need one byte.
    }
    msg[msg_len]='\n';
    output(msg,msg_len+1);

    ClearCurrentTm();
}

void CLogger::writeLog(const char* file,int line,const char* func,int level,const char* fmt,...)
{
    va_list args;
    va_start(args,fmt);
    writeLog(file,line,func,level,fmt,args);
    va_end(args);
}

void CLogger::output(char* msg,size_t size)
{
    if(pAppender_){
        pAppender_->output(msg,size);
    }
}

CLogger* CLogger::dummyLogger;

map<string,LoggerPtr> CLogger::logMap_;
boost::mutex CLogger::mutex_;
map<string,Appender*> CLogger::appenderMap_;
LoggerPtr CLogger::pWfLogger_;

const char* LEVEL_STR[LEVEL_ALL]={
    "NONE","FATAL","WARN","ERROR","NOTICE","TRACE","LOG","INFO","DEBUG",
};
