#include "http_connect.h"

//定义一些HTTP响应的一些状态信息
const char * ok_200_title="OK";
const char * error_400_title="Bad Request";
const char * error_400_form="Your request has bad syntax or is inherently impossable to satisfy.\n";
const char * error_403_title="Forbidden";
const char * error_403_form="You do not have permission to get file from this server.\n";
const char * error_404_title="Not Found";
const char * error_404_form="The request file was not found on this srever.\n";
const char * error_500_title="Internal Error";
const char * error_500_form="There was an unusual problem serving the requested file.\n";
//网站的根目录
const char * doc_root="/var/www/html";

int setnonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    if (one_shot)
    {
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLONESHOT|EPOLLET|EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;

void http_conn::close_conn(bool real_close)
{
    if(real_close&&(m_sockfd!=-1))
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        --m_user_count;  //关闭一个连接时,讲客户总量减1
    }
}

void http_conn::init(int sockfd,const sockaddr_in & addr)
{
    m_sockfd=sockfd;
    m_address=addr;
    //以下两行是为了避免TIME_wAIT状态,仅用于调试,实际使用应去除
    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

void http_conn::init()
{
    m_check_state=CHECK_STATE_REQUESTLINE;
    m_linger=false;

    m_method=GET;
    m_url=0;
    m_version=0;
    m_content_length=0;
    m_host=0;
    m_start_line=0;
    m_check_idx=0;
    m_read_idx=0;
    m_write_idx=0;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

//从状态机
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_check_idx<m_read_idx;++m_check_idx)
    {
        temp=m_read_buf[m_check_idx];
        if(temp=='\r')
        {
            if(m_check_idx+1==m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_check_idx+1]=='\n')
            {
                m_read_buf[m_check_idx++]='\0';
                m_read_buf[m_check_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp=='\n')
        {
            if(m_check_idx>1&&m_read_buf[m_check_idx-1]=='\r')
            {
                m_read_buf[m_check_idx-1]='\0';
                m_read_buf[m_check_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据,知道无数据可读或者对方关闭连接
bool http_conn::read()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read=0;
    while(true)
    {
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1)
        {
            if(errno==EAGAIN||errno==EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read==0)
        {
            return false;
        }
        m_read_idx+=bytes_read;
    }
    return true;
}

//解析HTTP请求行,获得请求方法.目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char * text)
{
    //查找和目标字符有一个相同的第一个位置.没有返回NULL
    m_url=strpbrk(text," \t");
    if(!m_url)
        return BAD_REQUEST;
    *m_url++='\0';

    char * method=text;
    //比较两个字符串是否相同
    if(strcasecmp(method,"GET")==0)
    {
        m_method=GET;
    }
    else
    {
        return BAD_REQUEST;
    }
    //返回m_url字符串开头连续不含字符串 \t 内的字符数目.
    m_url+=strspn(m_url," \t");
    m_version=strpbrk(m_url," \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++='\0';
    m_version+=strspn(m_version," \t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
     //比较两个字符串前n个字符是否相同
    if (strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        //strchr函数功能为在一个串中查找给定字符的第一个匹配之处
        m_url=strchr(m_url,'/');
    }

    if(!m_url||m_url[0]!='/')
    {
        return BAD_REQUEST;
    }
    
    m_check_state=CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char * text)
{
    if(text[0]=='\0')
    {
        //遇到空行,说明头部字段解析完毕
        //如果HTTP请求有消息体,则还需要读取m_content_length字节的消息体,状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length!=0)
        {
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到一个完整的HTTP请求
        return GET_REQUEST;
    }
    //处理Connection头部字段
    else if (strncasecmp(text,"Connection:",11)==0)
    {
        text+=11;
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            m_linger=true;
        }
    }
    //处理Content-Length头部字段
    else if (strncasecmp(text,"Content-Length",15)==0)
    {
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atoi(text);
    }
    //处理Host头部
    else if (strncasecmp(text,"Host:",5)==0)
    {
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    else
    {
        printf("oop! unkow header %s\n",text);
    }
    return NO_REQUEST;
}

//我们没有真正解析HTTP请求的消息体,只是判断它是否被完整地读入了
http_conn::HTTP_CODE http_conn::parse_content(char * text)
{
    if(m_read_idx>=(m_content_length+m_check_idx))
    {
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char * text=NULL;

    return NO_REQUEST;
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char * text=0;

    while ((m_check_state==CHECK_STATE_CONTENT)||(line_status==LINE_OK)||((line_status=parse_line())==LINE_OK))
    {
        text=get_line();
        m_start_line=m_check_idx;
        printf("got 1 http line: %s\n",text);

        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {   
                ret=parse_request_line(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret=parse_headers(text);
                if (ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if (ret==GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret=parse_content(text);
                if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    
    return NO_REQUEST;
}

//当得到一个完整的正确的HTTP请求,我们分析目标文件的属性,如果目标文件存在,对所有用户可读,且不是目录,则使用mmap将其映射到内存地址m_file_address处,并告诉调用者读取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    if(stat(m_real_file,&m_file_stat)<0)
    {
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode&S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    int fd=open(m_real_file,O_RDONLY);

    m_file_address=(char *)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);

    close(fd);
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

//写http响应
bool http_conn::write()
{
    int temp=0;
    int bytes_have_send=0;
    int bytes_to_send=m_write_idx;
    if(bytes_to_send==0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        temp=writev(m_sockfd,m_iv,m_iv_count);
        if(temp<=-1)
        {
            //如果TCP写缓冲没有空间,则等待下一轮EPOLLOUTT事件,虽然在此期间,服务器无法立即接收同一客户的下一个请求,但这可以保证连接的完整性
            if(errno==EAGAIN)
            {
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send-=temp;
        bytes_have_send+=temp;
        if(bytes_to_send<=bytes_have_send)
        {
            //发送HTTP响应,根据HTTP请求中的Connection字段决堤是否立即关闭连接
            unmap();
            if(m_linger)
            {
                init();
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            else
            {
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return false;
            }
        }
    }
}

