#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <sstream>
#include "FileAppender.h"
#include "StrUtil.h"
using namespace std;


static inline void CreateDir(std::string &path) {
    std::string::size_type off = 0;

    while ((off = path.find("/", off)) != std::string::npos)  {
        char backup = path[off];
        
        path[off] = 0;
        mkdir(path.c_str(), 0755);
        path[off] = backup;
        
        ++ off;
    }
}

FileAppender::FileAppender(const string& path,uint64_t splitsize,const string& splitFormat)
    :Appender(path),path_(path),splitSize_(splitsize),checkTimeInterval_(1),lastcheck_(0),fd_(-1),splitFormat_(splitFormat)
{
    CreateDir(path_);
    fd_ = open(path_.c_str(), O_CREAT | O_WRONLY | O_LARGEFILE, 0755);
    if(fd_ < 0){
        // 先占个坑，虽然我现在无法打开文件，比如因为路径权限问题，我先把日志刷到stderr
        // 运行过程中人工修改路径权限，我又能打开文件了，那时候checkFile过程中，fd_原子性指向新文件，我就能写了
        fd_ = dup(2);
    }
    
    if(splitSize_ <= 0){
        splitSize_ = DEFAULT_SPLITSIZE;
    }
    else if(splitSize_ < MIN_SPLITSIZE){
        splitSize_ = MIN_SPLITSIZE;
    }
    else if(splitSize_ > MAX_SPLITSIZE){
        splitSize_ = MAX_SPLITSIZE;
    }

    generateSplitFormatCharMap();
    parseSplitFormat();
}

void FileAppender::generateSplitFormatCharMap()
{
    formatsCharMap_['Y']=format_t::YEAR;
    formatsCharMap_['m']=format_t::MONTH;
    formatsCharMap_['d']=format_t::DAY;
    formatsCharMap_['H']=format_t::HOUR;
    formatsCharMap_['M']=format_t::MINUTE;
    formatsCharMap_['S']=format_t::SECOND;
    formatsCharMap_['P']=format_t::PID;
    formatsCharMap_['n']=format_t::SEQNUMBER;
}

void FileAppender::parseSplitFormat()
{
    
    string::size_type pos = 0;
    string::size_type find_pos = 0;
    string::size_type use;
    string tmp;
    
    use = splitFormat_.find('%');
    while(use != string::npos){
        assert(pos <= use);
        format_t fmt;
        if(use+1 != string::npos){
            charToFormatMap::iterator it=formatsCharMap_.find(splitFormat_[use+1]);
            if(it!=formatsCharMap_.end()){
                tmp = splitFormat_.substr(pos,use-pos);
                if(!tmp.empty()){
                    fmt.str_ = tmp;
                }
                fmt.type_ = formatsCharMap_[splitFormat_[use+1]];
                formats_.push_back(fmt);
                pos = use+2;
                find_pos = pos;
            }
            else{
                find_pos = use+1;
            }
        }
        else{
            //%H%m%d_xxx% 最后一个字符就是%
            fmt.type_ = format_t::RAW_STR;
            fmt.str_ = splitFormat_.substr(pos,splitFormat_.length()-pos);
            formats_.push_back(fmt);
            break;
        }
        
        use = splitFormat_.find('%',find_pos);
    }
    format_t fmt;
    // %H%m%dxx 最后是RAW_STR
    if(pos != string::npos){
        fmt.type_ = format_t::RAW_STR;
        fmt.str_ = splitFormat_.substr(pos,splitFormat_.length()-pos);
        formats_.push_back(fmt);
    }
}

FileAppender::~FileAppender()
{
    if(fd_ >= 0)
        ::close(fd_);
}

void FileAppender::reopen()
{
    // if fd == -1,没有效果，也没有任何副作用。fd_将仍然指向旧文件。下次检查还会发现inode变化，继续尝试。
    // 如果open正在操作目标fd，也就是open返回fd_一样的数值，dup2返回 EBUSY。但是因为我们保证了fd_始终有效，open必然不会返回fd_一样的数值。所以dup要么成功，要么因为fd==-1而失败，如上所说，fd=-1没有影响。
    // dup2是原子的。利用这一点，我们不用多线程加锁了，这一技巧来自淘宝的开源tbcommon-utils中的tblog，感谢多隆大神。

    int fd = open(path_.c_str(), O_CREAT | O_WRONLY | O_LARGEFILE, 0755);
    if(fd < 0 && errno == ENOENT){
        CreateDir(path_);
        fd = open(path_.c_str(), O_CREAT | O_WRONLY | O_LARGEFILE, 0755);
    }
    dup2(fd, fd_);
    close(fd);

    // 还有一点要说明，同一个进程，多次open同一个文件，每次fd不同。幸亏是这样，否则如果文件一样fd就一样的话，fd=open();close(fd);多线程并发情况下，我们可能会多次close同一个fd，而这个fd在两次close之间有可能被open成了其他设备，甚至可能是一个网络fd，那就悲剧了。
}

string FileAppender::getNewFilename(time_t now,int seqNumber)const
{
    struct tm newTimeObj;
    localtime_r(&now,&newTimeObj);
    char tmpStr[16]={0};
    
    // 如同date命令的格式%Y%m%dH%M%S
    // %P进程号，%n序列号，从000开始，如果同名文件存在，自动累加直到找到一个可用名字，上限999. example: a.log_%Y%m%d%H%M%S_%P_%n,如果a.log.20121010090048_3164_000被占用，会去看a.log.20121010090048_3164_001是否可用
    stringstream stream;
    stream<<path_;
    
    for (vector<format_t>::const_iterator it=formats_.begin(); it != formats_.end(); ++it)
    {
        switch(it->type_){
            case format_t::RAW_STR:
                stream<<it->str_;
                break;
                
            case format_t::YEAR:
                snprintf(tmpStr,sizeof(tmpStr),"%4d",newTimeObj.tm_year+1900);
                stream<<it->str_;
                stream<<tmpStr;
                break;
            
            case format_t::MONTH:
                snprintf(tmpStr,sizeof(tmpStr),"%02d",newTimeObj.tm_mon+1);
                stream<<it->str_;
                stream<<tmpStr;
                break;

            case format_t::DAY:
                snprintf(tmpStr,sizeof(tmpStr),"%02d",newTimeObj.tm_mday);
                stream<<it->str_;
                stream<<tmpStr;
                break;

            case format_t::HOUR:
                snprintf(tmpStr,sizeof(tmpStr),"%02d",newTimeObj.tm_hour);
                stream<<it->str_;
                stream<<tmpStr;
                break;

            case format_t::MINUTE:
                snprintf(tmpStr,sizeof(tmpStr),"%02d",newTimeObj.tm_min);
                stream<<it->str_;
                stream<<tmpStr;
                break;

            case format_t::SECOND:
                snprintf(tmpStr,sizeof(tmpStr),"%02d",newTimeObj.tm_sec);
                stream<<it->str_;
                stream<<tmpStr;
                break;

            case format_t::PID:
                snprintf(tmpStr,sizeof(tmpStr),"%d",getpid());
                stream<<it->str_;
                stream<<tmpStr;
                break;

            case format_t::SEQNUMBER:
                snprintf(tmpStr,sizeof(tmpStr),"%03d",seqNumber);
                stream<<it->str_;
                stream<<tmpStr;
                break;

            default:
                fprintf(stderr,"%s(%s:%d) fatal error,fileAppender met unknown format type!",__func__,__FILE__,__LINE__);
                abort();
                break;
        }
    }
    return stream.str();
}


void FileAppender::checkFile()
{
    // 隔一定次数检查一次是否需要重新打开，每次都检查效率太低.文件切分大小有误差，这个问题不大
    struct timeval* tm = TestSetCurrentTm();    
    time_t now = tm->tv_sec;
    {
        // 此处用原子操作也可实现。当前我厂gcc/g++版本陈旧，没有__sync_lock_test_and_set这种高级货色
        // linux自带的atomic还不够。我也不想用汇编搞一段，那个有损移植性.
        boost::mutex::scoped_lock guard(checkMutex_);
        if(lastcheck_ + checkTimeInterval_ > now){
            return;
        }
        lastcheck_ = now;
    }
    
        
// 1.检查当前fd是否还指向log文件，不是则说明被mv或者rm了，重新open(path,O_CREAT|O_WRONLY|O_APPEND,0755)
// 2.检查log文件的大小，如果已经超过切分大小，mv，open
    struct stat stFile;
    struct stat stFd;
    memset(&stFd,0,sizeof(stFd));
    memset(&stFile,0,sizeof(stFile));
    
    // 第一次保证打开日志文件获取有效fd_.之后只有成功open了新文件才会dup到fd_上去。保证fd_一直是有效的。
    fstat(fd_, &stFd);
    int err = stat(path_.c_str(), &stFile);
    if ((err == -1 && errno == ENOENT)
        || (err == 0 && (stFile.st_dev != stFd.st_dev || stFile.st_ino != stFd.st_ino))) {

        // 仔细阅读reopen说明。
        reopen();
        
        // 再次stat，获取文件信息供检查使用
        if((err = stat(path_.c_str(),&stFile)) < 0){
            return;
        }
    }
    
    if(err == 0 && static_cast<uint64_t>(stFile.st_size) >= splitSize_){
        // mv reopen.
        string newFileName;
        bool foundFileName=false;//是否找到一个可用的文件名
        for (int surfix = 0; surfix < 1000; ++surfix)
        {
            newFileName=getNewFilename(now,surfix);
            struct stat checkBuf;
            if(stat(newFileName.c_str(),&checkBuf) < 0){
                foundFileName=true;
                break;          // 找到一个不存在的文件名
            }
            // 注意上面的检查文件是否存在和这个rename不是原子操作，可能在这个时间窗口内被人为的创建出来一个重名的文件，但是程序本身不会，因为newFileName是加了pid的。概率实在非常非常小了，可以忽略。
        }
        // rename,reopen
        if(foundFileName){
            rename(path_.c_str(),newFileName.c_str());
            reopen();
        }
        else{
            // shit,试了1000次居然都被占用了，只好继续打到原来的文件，过一会再尝试切分，那时时间变了，或许有可用文件名了
            // do nothing
        }        
    }
}

void FileAppender::output(char* msg,size_t len)
{
    checkFile();
    
    ::write(fd_,msg,len);
    free(msg);
}

void FileAppender::outputWithoutLock(char* msg,size_t len)
{
    checkFile();
    
    ssize_t left=len;
    do{
        ssize_t nwrite =  ::write(fd_,msg+(len-left),left);
        if(nwrite >= 0){
            left -= nwrite;
        }
        else{
            // error.
            fprintf(stderr,"error=write_fail logger=%s filepath=%s fd=%d errno=%d log=%s\n",this->getAppenderName().c_str(),path_.c_str(),fd_,errno,msg);
            break;
        }
    }while(left>0);
}
