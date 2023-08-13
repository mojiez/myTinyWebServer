#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/parallels/Desktop/CProject/tinyWebServer/TinyWebServer-raw_version/root";

// 将表中的用户名和密码放入map
map<string,string> users;
locker m_lock;

// 初始化连接 外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr){
    // 应该是与客户端交流的套接字 和 客户端的地址 accept函数能得到客户端的地址
    m_sockfd = sockfd;
    m_address = addr;

    // TODO
    addfd(m_epollfd,sockfd,true);

    m_user_count++;
    init();
}

// 私有成员函数init
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;

    // m_read_idx是啥？
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 读取浏览器端发来的全部数据 
bool http_conn::read_once()
{
    // if(m_read_idx>=READ_BUFFER_SIZE)
    // {
    //     return fasle;
    // }

    // int bytes_read = 0;
    
// #ifdef connfdLT
//     bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
//     if(bytes_read<0){
//         return false;
//     }
//     return true;
// #endif

    // 读取浏览器发来的全部数据
    
    // m_read_idx就是读了多少个字符
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    
    // 这一次recv接收了多少字符 用bytes_read记录
    int bytes_read = 0;
    bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
    if(bytes_read <= 0)
    {
        return false;
    }
    m_read_idx += bytes_read;
    return true;


}

// 从状态机 分析一行的内容
// 返回值为行的读取状态，有LINE_OK LINE_BAD LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;

    for(;m_checked_idx<m_read_idx;m_checked_idx++)
    {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r')
        {
            // 读了100个字符 序号是0-99
            if((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }

            else if(m_read_buf[m_checked_idx+1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            
            // 其他情况：后面还有字 但是下一个字不是\n
            return LINE_BAD;
        }
        else if(temp == '\n')
        {
            // 为什么要 >1 ？？ TODO 
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            // \n单独出现 也有问题
            return LINE_BAD;
        }
    }

    // 如果说这次都没有出现 \r \n那说明肯定没有读完
    return LINE_OPEN;
}

// 创建连接套接字后 后续由客户端发来的http请求工作线程是怎么处理的？
// 就是直接调用这个对应的对象的process函数
void http_conn::process()
{
    // 先解析请求 process_read();
    HTTP_CODE read_ret = process_read();

    if(read_ret == NO_REQUEST)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);

    // 如果写不成功 就关闭连接
    if(!write_ret)
    {
        close_conn();
    }

    // 将这个套接字重新加入epoll监听 表示可以进行写操作
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}

http::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 如果read_buf中能读出来一行 或者 之前读的一行没问题然后接下来应该读请求体 这两种情况下就继续读
    // 在GET请求报文中 每一行都是\r\n作为结束 
    // 在post请求报文中 消息体的末尾没有任何字符 因此从状态机在解析消息体时返回的状态是LINE_OPEN 没读完
    // 因此需要使用主状态机的状态
    // 如果只使用 m_check_state == CHECK_STATE_CONTENT的话 
    // 当第一次解析完消息体时 ，主状态机的状态不变 也就是说还会进入循环 
    // 但是此时从状态机的状态变为LINE_OPEN 所以正确判断语句为：
    // line_status == LINE_OK && m_check_state == CHECK_STATE_CONTENT
    while((line_status = parse_line() == LINE_OK)||(line_status == LINE_OK && m_check_state == CHECK_STATE_CONTENT))
    {
        // m_start_line 记录read_buf中还没有check的行的第一个位置
        // char* getline() { return m_read_buf + m_start_line; };
        
        text = get_line(); // 获取要处理的这个字符串的首地址 m_start_line 保存
        // 更新m_start_line

        m_start_line = m_checked_idx;

        // LOG TODO
        
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                // 
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_header(text);
                // 读一行 状态没有改变 那就会继续读
                if(ret == BAD_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                // 
                if(ret == GET_REQUEST)
                    return do_request();
                // 请求体解析一次后将从状态机的状态变为 LINE_OPEN 这里是保险吧
                line_status = LINE_OPEN;
                break;
            }
            default:
                // 服务器内部错误 一般不会触发
                return INTERNAL_ERROR;
        }
        

        // 如果没有执行 parse_line()的话 m_start_line 和 m_checked_idx 大小一样
        // 如果执行了的话 
    }

    // 不进循环的情况
    // read_buf中下一行读不出来了 而且也不是解析请求体
    // 也没有读完 如果读完直接就do_request了 
    // 那就是请求不完整 需要继续读取请求报文数据
    return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 查找空格
    // strbrk()
    // 传入两个字符串，在第一个字符串中查找匹配第二个字符串中任意字符的位置 ，并返回这个位置的指针

    m_url = strpbrk(text," \t");
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    
    // 先把m_url所指的字符设置为终止字符\0 
    // 然后再将指针m_url向后移动一个位置
    *m_url = '\0';      
    // 递增指针是如何实现的？？
    // c++中，递增指针实际上是通过增加指针所指向的数据类型的字节数来实现的
    // 具体来说 递增指针会根据指针所指向的数据类型的大小，在内存中向前移动响应的字节数
    // 例如 一个指向int类型的指针在递增时会移动sizeof(int)个字节 指向下一个int
    // 如果在递增
    m_url++;

    char *method = text;

    if(strcasecmp(method,"GET" == 0)) m_method = GET;
    else if(strcasecmp(method,"POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else return BAD_REQUEST;

    //strspn()功能是计算str字符串开始部分连续包含" \t"字符串中字符的长度 
    //换句话说就是计算str字符串开头有多少个字符是属于" \t"字符串中的字符 
    //这里的目的还是跳过空格 将m_url指向
    m_url += strspn(m_url, " \t");

    //同理 找版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';

    // 再次跳过空格
    m_version += strspn(m_version, " \t");

    // 如果协议的类型不是HTTP/1.1的话 也说明整个HTTP请求有问题
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    
    // 如果url前7个字符是 http://
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //strchr()的作用是在m_url字符串中查找第一个出现 '/' 的位置 并返回这个位置的指针
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 如果现在m_url为空或者m_url的第一位不是/ （也就是找不到的情况）那http请求还是错的
    // 也就是说url必须是这样的形式： http://xxxxx/....
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1) //如果现在请求的长度是1 意味着URL现在只包含了一个字符（/） 这时候表示请求的是服务器的根目录 (默认目录)
        strcat(m_url, "judge.html"); //HTTP中根目录通常是特定的某个文件 例如index.html 这里是 judge.html
        //strcat()函数将judge.html拼接到m_url的末尾 从而构造了一个完整的路径名字

    // 如果指定了访问的路径名的话 不做修改(不用指定默认的路径)    
    // 请求行弄完 将主状态机的状态修改为 请求头
    
    m_check_state = CHECK_STATE_HEADER;
    //返回当前http请求是 请求不完整 (请求头那些都还没弄完 请求当然不完整)
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_header(char *text)
{
    // 请求头是有很多行的
    // 如果为空 
    if(text[0] == '\0');
    {
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    // text指针 指向要处理的字符串的首地址
    // 如果当前的text不为空 那么就开始解析：
    // Connection Content-length Host还有其他类型
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }

    // m_content_length的长度就是这么来的 一开始是0
    // 请求头中有一个字段是请求体的长度 如果请求体的长度不为零 状态转移为CHECK_STATE_CONTENT
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    // 弄完一行 直接标成请求不完整 直到text为空
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if(m_read_idx >= (m_content_length + m_checked_idx))
    {
        // m_read_idx就是m_read_buf中有多少个字符 
        // m_checked_idx是现在已经解析了的字符 
        // 如果全部的字符大于或者等于 已经解析的字符+将要解析的字符的话

        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    
    // 如果是全部的字符小于已经解析的字符+将要解析的字符 
    // 说明还有数据没发完
    return NO_REQUEST;
}

// do_request就是根据之前解析的响应报文 进一步解析 拼出来要访问的文件的路径 最后把这个文件映射到内存
http_conn::HTTP_CODE http_conn::do_request()
{
    // 生成响应报文 并发回给客户端

    // 对于请求打开某个文件的 要把文件地址映射到内存上 提高访问速度

    // 将doc_root复制到m_real_file
    // 将doc_root复制到m_real_file

    // m_real_file就是要访问的文件的具体路径
    strcpy(m_real_file, doc_root);

    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);

    //查找最后一个 / 出现的位置 并返回这个位置的指针
    const char *p = strrchr(m_url, '/');

    //处理cgi

    // GET请求cgi为0 POST请求cgi为1
    // 判断是登陆还是注册 登陆和注册都是POST请求
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }


    // 注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        //strcpy 拷贝
        strcpy(m_url_real, "/register.html");

        //strncpy 拷贝字符串的前n个字符 所以要多传入一个参数 strcpy
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //登陆界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    // 图片界面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    // 视频界面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    // fans.html
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //stat()函数用来获取文件的状态信息 包括文件的大小、访问权限、修改时间等
    //第一个参数是文件路径
    //第二个参数是指向struct stat结构体的指针，表示将这个文件的状态信息存储在变量m_file_stat所指向的内存中
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);

    //得到这个文件在内存中的逻辑地址？？
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    // 关闭这个文件描述符 避免文件描述符的占用和浪费
    close(fd);

    // 表示请求的文件存在， 且可以访问
    return FILE_REQUEST;
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:
        {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
        }
        case BAD_REQUEST:
        {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
        }
        case FORBIDDEN_REQUEST:
        {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
        }


        // 正确的报文：
        case FILE_REQUEST:
        {
            // 状态码200 ok
            add_status_line(200,ok_200_title);

            // 如果说有实际的文件内容需要返回
            if(m_file_stat.st_size != 0)
            {
                // 添加头部信息 指定响应的内容长度为 这个文件的大小
                add_headers(m_file_stat.st_size);

                // 使用iove结构体构造相应数据 m_iv[0]表示响应的头部信息
                // m_iv[1]表示文件的内容
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;

                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;

            }
            // 不需要文件
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }

        default:
            return false;
    }
    
    // 这里的代码好像没什么用
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 将响应报文发送给浏览器端的函数
bool http_conn::write()
{
//     temp = writev(m_sockfd,m_iv,m_iv_count);

//     if(temp < 0)
//     {

//         if(errno == EAGAIN)
//         {
//             modfd(m_epollfd,m_sockfd,EPOLLOUT);
//             return true;
//         }

//         // 如果是发送失败的话 就解除文件再内存的映射 返回false
        
//         // unmap() TODO
//         return false;
//     }
    int temp = 0;

    // 如果待发送的数据为0 说明已经完成了报文请求和报文解析 那么这个http对象就可以进行下一次请求的处理了 那就初始化
    // 这时候的初始化还是会保留这个连接套接字

    if(bytes_to_send == 0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        // 将响应数据写入套接字
        temp = writev(m_sockfd,m_iv,m_iv_count);

        if(temp < 0)
        {
            if(errno == EAGAIN)
            {
                // 表示缓冲区已经写满 需要等待下次可以写的状态
                // 重新去监听
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }

            // 如果是发送失败 就解除文件在内存的映射 返回false
            unmap();
            return false;
        }

        // 正常发送的流程：：
        bytes_have_send += temp;
        bytes_to_send -= temp;


        // 调整偏移量
        if(bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            // 重新制定地址 = 文件数据的起始地址 + (已经发送的总字节数 - m_iv[0].iov_len的长度)
            // = 文件数据的起始地址 + 已经发送的总字节数 - [0]所占的字节数 
            // m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 发完了
        if(bytes_to_send <= 0)
        {
            // unmap()  TODO
            modfd(m_epollfd,m_sockfd,EPOLLIN); // 重新投入使用

            if(m_linger)//如果是持久性连接
            {
                init();
                return true;
            }
            // 为什么如果不是持久性连接的话要return false?
            // 接收到false 可能说明这个连接需要关闭！！！
            else
            {
                return false;
            }
        }
    }
}