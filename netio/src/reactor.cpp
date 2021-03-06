/*
 * reactor.cpp

 *
 *  Created on: 2015年12月23日
 *      Author: chenzhuo
 */
#include "reactor.h"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <sys/stat.h>
#include "global_define.h"
#include "assert.h"
#include "string_helper.h"
#include "cmd_obj.h"


//CNetHandler::~CNetHandler() { }

CReactor::CReactor() : m_pTcpNetHandler(NULL),m_pUSockUdpHandler(NULL),m_iSvrFd(0),m_iUSockFd(0),m_iEpollSucc(0),m_pUserEventHandler(NULL) {
	m_pszBuf=new char[10240];
	m_iEvents = 0;

}
CReactor::~CReactor() {
	delete m_pszBuf;
	close(m_iSvrFd);
	close(m_iUSockFd);
	if (m_pTcpNetHandler) {
		delete m_pTcpNetHandler;
	}
	if (m_pUSockUdpHandler) {
		delete m_pUSockUdpHandler;
	}
}

int CReactor::InitTcpSvr(int iTcpSvrPort) {
	/*
	 * struct sockaddr_in

	 {

	 short sin_family;/Address family一般来说AF_INET（地址族）PF_INET（协议族）

	 unsigned short sin_port;//Port number(必须要采用网络数据格式,普通数字可以用htons()函数转换成网络数据格式的数字)

	 struct in_addr sin_addr;//IP address in network byte order（Internet address）

	 unsigned char sin_zero[8];//Same size as struct sockaddr没有实际意义,只是为了　跟SOCKADDR结构在内存中对齐

	 };
	 */
	struct sockaddr_in stServaddr;
	int iRet = 0;
	m_iSvrFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_iSvrFd < 0) {
		return CREATE_SOCKET_FAILED;
	}
	bzero(&stServaddr, sizeof(stServaddr));
	stServaddr.sin_family = AF_INET;
	stServaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	stServaddr.sin_port = htons(iTcpSvrPort);
	int opt = 1;
	//SO_REUSEADDR 决定time_wait的socket能否被立即重新绑定。同时允许其它若干非常规重复绑定行为，不详细展开了
	iRet = setsockopt(m_iSvrFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (iRet < 0) {
		close(m_iSvrFd);
		return SET_SOCKOPT_FAILED;
	}

	iRet = bind(m_iSvrFd, (struct sockaddr *) &stServaddr, sizeof(stServaddr));
	if (0 != iRet) {
		close(m_iSvrFd);
		return BIND_SOCKET_FAILED;
	}

	//SO_LINGER 决定系统如何处理残存在套接字发送队列中的数据。处理方式无非两种，丢弃和发送到对端后优雅关闭
	/*typedef struct linger {
	 u_short l_onoff; //是否开启，默认是0表示关闭
	 u_short l_linger; //优雅关闭最长时限
	 }
	 当l_onoff为0时，整个结构无意义，表示socket继续沿用默认行为。closesocket立即返回，发送队列系统底层保持至发送到对端完成
	 l_onoff非0，l_linger=0 表示立即丢弃数据，直接发送rst包，自身复位
	 l_onoff非0，l_linger非0，当是阻塞式socket时，closesocket阻塞到l_linger时间超时或数据发送完成。超时后，发送队列还是会被丢弃
	 */
	linger stLinger;
	stLinger.l_onoff = 1;
	stLinger.l_linger = 0;
	iRet = setsockopt(m_iSvrFd, SOL_SOCKET, SO_LINGER, &stLinger, sizeof(stLinger));
	if (iRet < 0) {
		close(m_iSvrFd);
		return SET_SOCKOPT_FAILED;
	}

	int iBackLog = 20;
	iRet = listen(m_iSvrFd, iBackLog);
	if (iRet < 0) {
		close(m_iSvrFd);
		return LISTEN_SOCKET_FAILED;
	}

	int iCurrentFlag = 0;
	iCurrentFlag = fcntl(m_iSvrFd, F_GETFL, 0);
	if (iCurrentFlag == -1) {
		close(m_iSvrFd);
		return FCNTL_SOCKET_FAILED;
	}

	iRet = fcntl(m_iSvrFd, F_SETFL, iCurrentFlag | O_NONBLOCK); //设置非阻塞
	if (iRet < 0) {
		close(m_iSvrFd);
		return FCNTL_SOCKET_FAILED;
	}
	stTcpSockItem stTcpSock;
	stTcpSock.fd = m_iSvrFd;
	stTcpSock.enEventFlag = TCP_SERVER_ACCEPT;
	stTcpSock.stSockAddr_in = stServaddr;
	m_arrTcpSock[m_iSvrFd] = stTcpSock;

	return 0;
}

int CReactor::InitUSockUdpSvr(const char* pszUSockPath) {
	//printf("InitUSockUdpSvr\n");
	/*
	 * AF 表示ADDRESS FAMILY 地址族;PF 表示PROTOCL FAMILY 协议族
	 * AF_UNIX=AF_LOCAL, PF_UNIX=PF_LOCAL.如果看到usock初始化写法时用_UNIX也不用惊奇。不同的unix分支与linux可能有所区别
	 * 但可以通过公共网络库的来处理跨平台的问题，而不是如下这样绑定在linux中
	 *
	 * 一般的建议是
	 * 对于socketpair与socket的domain参数,使用PF_LOCAL系列,而在初始化套接口地址结构时,则使用AF_LOCAL.
	 */
	m_iUSockFd = ::socket(PF_LOCAL, SOCK_DGRAM, 0);
	if (m_iUSockFd < 0) {
		return CREATE_USOCK_FAILED;
	}

	mode_t old_mod = umask(S_IRWXO); // Set umask = 002
	/*
	 * struct sockaddr_un {
               sa_family_t sun_family;               // AF_UNIX
               char        sun_path[108];            // pathname
       };
	 */
	struct sockaddr_un stServaddr;
	bzero(&stServaddr, sizeof(stServaddr));
	stServaddr.sun_family = AF_LOCAL;
	strncpy(stServaddr.sun_path, pszUSockPath, sizeof(stServaddr.sun_path));

	int opt = 1;
	int iRet = 0;
	iRet = setsockopt(m_iUSockFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (iRet < 0) {
		close(m_iUSockFd);
		return SET_USOCKOPT_FAILED;
	}
	unlink(pszUSockPath); // unlink it if exist
	//printf("unlink-iRet:%d\n",iRet);


	/*
	 * #define SUN_LEN(su) \
        (sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))
       因为sockaddr与sockaddr_un的顶部结构是兼容的，所以这个预定义长度不对等的转换也是没问题的
	 */
	iRet = bind(m_iUSockFd, (struct sockaddr *) &stServaddr, SUN_LEN(&stServaddr));
	if (0 != iRet) {
		close(m_iUSockFd);
		printf("iRet:%d,errno:%d,info:%s\n",iRet,errno,strerror(errno));
		return BIND_USOCKET_FAILED;
	}
	umask(old_mod);

	int iCurrentFlag = 0;
	iCurrentFlag = fcntl(m_iUSockFd, F_GETFL, 0);
	if (iCurrentFlag == -1) {
		close(m_iUSockFd);
		return FCNTL_SOCKET_FAILED;
	}

	iRet = fcntl(m_iUSockFd, F_SETFL, iCurrentFlag | O_NONBLOCK); //设置非阻塞
	if (iRet < 0) {
		close(m_iUSockFd);
		return FCNTL_SOCKET_FAILED;
	}

	stTcpSockItem stTcpSock;
	stTcpSock.fd = m_iUSockFd;
	stTcpSock.enEventFlag = UDP_READ;

	m_arrTcpSock[m_iUSockFd] = stTcpSock;

	return 0;
}

int CReactor::Init(int iTcpSvrPort, const char* pszUSockPath) {
	int iRet = 0;

	m_pszFlag[0] = "NONE_FLAG";
	m_pszFlag[1] = "TCP_CLIENT";
	m_pszFlag[2] = "TCP_SERVER_ACCEPT";
	m_pszFlag[3] = "TCP_SERVER_READ";
	m_pszFlag[4] = "TCP_SERVER_SEND";
	m_pszFlag[5] = "TCP_SERVER_CLOSE";
	m_pszFlag[6] = "UDP_READ";
	m_pszFlag[7] = "UDP_SEND";

	if (iTcpSvrPort) {
		iRet = InitTcpSvr(iTcpSvrPort);
		if (iRet) {
			return iRet;
		}
	}

	if (NULL != pszUSockPath) {
		iRet = InitUSockUdpSvr(pszUSockPath);
		if (iRet) {
			return iRet;
		}
	}

	//初始化Epoll数据部分
	m_iEpFd = ::epoll_create(MAX_EPOLL_EVENT_NUM); //加上空::表明是系统库函数或全局变量/函数，和成员函数明确区分开来
	if (-1 == m_iEpFd) {
		if (ENOSYS == errno) { //系统不支持
			assert(0);
		}
		return EPOLL_CREATE_FAILED;
	}

	m_arrEpollEvents = new epoll_event[MAX_EPOLL_EVENT_NUM];
	memset(m_arrEpollEvents, 0, MAX_EPOLL_EVENT_NUM * sizeof(epoll_event));

	m_bInit = 1;

	return 0;
}

int CReactor::AddToWatchList(int iFd, EventFlag_t emTodoOp, void* pData,bool bCheckSock) {


	stTcpSockItem* pStTempSock = reinterpret_cast<stTcpSockItem*>(pData);

	if (bCheckSock) {
		int iSockValid = 0;
		int inWatchList = 0;
		vector<int>::iterator it = m_vecFds.begin();
		//当socket信息还有效，才重新触发Epoll事件
		/*
		 * 1. 当监听队列中有此Fd，检查客户端等信息是否一致
		 * 2. 当监听队列中无此Fd, 检查传入的数据是否有效
		 * 3. 已关闭的fd，会将数据区的EventFlag置为无效值
		 */
		for (;it!=m_vecFds.end();++it) {
			if (iFd == (*it)) {
				inWatchList = 1;
				break;
			}
		}

		if (inWatchList) {
			printf("in watchlist:%d\n",iFd);
			if (m_arrTcpSock[iFd].enEventFlag == NONE_FLAG) {
				iSockValid = 0;

			}
			else if (pData != &m_arrTcpSock[iFd]) {
				iSockValid = 0;
				/*
				printf("---do match sin_port:%d\n",pStTempSock->stSockAddr_in.sin_port == m_arrTcpSock[iFd].stSockAddr_in.sin_port);
				printf("---do match sin_family:%d\n",pStTempSock->stSockAddr_in.sin_family == m_arrTcpSock[iFd].stSockAddr_in.sin_family);
				printf("---do match sin_addr:%d\n",(strcmp(inet_ntoa(pStTempSock->stSockAddr_in.sin_addr),inet_ntoa(m_arrTcpSock[iFd].stSockAddr_in.sin_addr)) == 0));*/
				//stTcpSockItem* pStTempSock = reinterpret_cast<stTcpSockItem*>(pData);
				if ((pStTempSock->stSockAddr_in.sin_port == m_arrTcpSock[iFd].stSockAddr_in.sin_port)
					&& (pStTempSock->stSockAddr_in.sin_family == m_arrTcpSock[iFd].stSockAddr_in.sin_family)
					&& (strcmp(inet_ntoa(pStTempSock->stSockAddr_in.sin_addr),inet_ntoa(m_arrTcpSock[iFd].stSockAddr_in.sin_addr)) == 0)) {
					iSockValid = 1;
				}
			}
		}
		else {
			if (m_arrTcpSock[iFd].enEventFlag == NONE_FLAG) {
				iSockValid = 0;
			}
			else if (pData != &m_arrTcpSock[iFd]) {
				iSockValid = 0;
				//stTcpSockItem* pStTempSock = reinterpret_cast<stTcpSockItem*>(pData);
				if ((pStTempSock->stSockAddr_in.sin_port == m_arrTcpSock[iFd].stSockAddr_in.sin_port)
						&& (pStTempSock->stSockAddr_in.sin_family == m_arrTcpSock[iFd].stSockAddr_in.sin_family)
						&& (strcmp(inet_ntoa(pStTempSock->stSockAddr_in.sin_addr),inet_ntoa(m_arrTcpSock[iFd].stSockAddr_in.sin_addr)) == 0)) {
					iSockValid = 1;
				}
			}
			else {
				iSockValid = 1;
			}
		}

		if (!iSockValid) {
			printf("invalid socket:%d, skip\n",iFd);
			return 0;
		}
	}


	/*
	 *
	 typedef union epoll_data { //一般填一个fd参数即可
	 	 void        *ptr;
	 	 int          fd;
	 	 uint32_t     u32;
	 	 uint64_t     u64;
	 } epoll_data_t;

 	 struct epoll_event {
          uint32_t     events;      // Epoll events
          epoll_data_t  data;        //User data variable
     };
	 */

	struct epoll_event event;
	/*
	 * EPOLLIN ：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）；
EPOLLOUT：表示对应的文件描述符可以写；
EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）；
EPOLLERR：表示对应的文件描述符发生错误；
EPOLLHUP：表示对应的文件描述符被挂断；
EPOLLRDHUP: 2.6.17内核当对端主动关闭socket时，会触发这个事件。之前是触发可读，当执行读收到EOF时，认为对方已关闭。
EPOLLET： 将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于默认的水平触发(Level Triggered)来说的。ET对一个事件（如可读），只会通知一次。但LT当事件持续（一直可读），会一直通知
EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里
	 */
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	if (TCP_SERVER_SEND == emTodoOp || UDP_SEND == emTodoOp) {
		event.events = EPOLLOUT | EPOLLET | EPOLLRDHUP;
	}

	/*
	stEpollItem stSockItem;
	stSockItem.fd = iFd;
	stSockItem.enEventFlag = emTodoOp;
	stSockItem.pData = pData;

	m_arrEpollItem[iFd] = stSockItem;*/

	//event.data.fd = iFd; //用自定义结构的ptr代替传统的fd来区分信息
	if (pData != &m_arrTcpSock[iFd]) {
		printf("pData != &m_arrTcpSock[%d]\n",iFd);
		m_arrTcpSock[iFd].enEventFlag = pStTempSock->enEventFlag;
		m_arrTcpSock[iFd].fd = iFd;
		m_arrTcpSock[iFd].stSockAddr_in = pStTempSock->stSockAddr_in;
		event.data.ptr = (void*)&m_arrTcpSock[iFd];
	}
	else {
		printf("%d use pData direct\n",iFd);
		event.data.ptr = pData;
	}

	/*
	EPOLL_CTL_ADD：注册新的fd到epfd中；
	EPOLL_CTL_MOD：修改已经注册的fd的监听事件；
	EPOLL_CTL_DEL：从epfd中删除一个fd；
	 */
	int op = EPOLL_CTL_ADD;
	int iEpollAdd = 1;

	for(vector<int>::iterator it2=m_vecFds.begin();it2 != m_vecFds.end(); ++it2) {
		if ((*it2) == iFd) {
			op = EPOLL_CTL_MOD;
			iEpollAdd = 0;
			break;
		}
	}

	/*int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)*/
	int iRet = epoll_ctl(m_iEpFd, op, iFd, &event);
	if (iRet <  0) {
		printf("epoll_ctl err %d,error:%d,info:%s\n",iRet,errno,strerror(errno));
		return EPOLL_CNTL_FAILED;
	}

	if (EPOLL_CTL_MOD != op) {
		++m_iEvents;
		m_vecFds.push_back(iFd);
	}

	printf("###watch fd %d,iEventCount:%d,op:%s,epollAct:%s\n",iFd,m_iEvents, GetEventFlag(emTodoOp),op == EPOLL_CTL_MOD ? "modify" : "add");

	return 0;
}

const char* CReactor::GetEventFlag(int iFlag) {
	return m_pszFlag[iFlag];
}

int  CReactor::RemoveFromWatchList(int iFd) {
	//printf("###unwatch fd %d\n",iFd);
	int iRet = epoll_ctl(m_iEpFd, EPOLL_CTL_DEL, iFd, NULL);
	if (iRet <  0) {
		printf("###unwatch fd %d failed,err %d\n",iFd,iRet);
		return EPOLL_CNTL_FAILED;
	}
	vector<int>::iterator it = m_vecFds.begin();
	for (;it!=m_vecFds.end();++it) {
		if (iFd == (*it)) {
			m_vecFds.erase(it);
			--m_iEvents;
			break;
		}
	}
	printf("###unwatch fd %d，then m_vecFds count:%d,m_iEvents:%d\n",iFd,m_vecFds.size(),m_iEvents);

	return 0;
}
/**
 * 当前的实现是用户事件handler回调函数一定会触发进入，socket是依赖于事件的。返回0表示总体无事要做。
 */
int CReactor::CheckEvents() {

	//if (m_iEvents > 0) {//进程间通信也放入监听。当container有数据返回到来时，可发数据包及时唤醒
	//m_iEpollSucc = epoll_wait(m_iEpFd, m_arrEpollEvents, m_iEvents, DEFAULT_EPOLL_WAIT_TIME);
		m_iEpollSucc = epoll_wait(m_iEpFd, m_arrEpollEvents, 100, DEFAULT_EPOLL_WAIT_TIME); //超时时间单位是毫秒
		//printf("event count %d\n",m_iEpollSucc);
		if (m_iEpollSucc < 0) { //出错的时候返回-1，可通过errno查看具体错误.否则返回可处理的IO个数
			printf("###event wait failed %d\n",m_iEpollSucc);
			m_iEpollSucc = 0;;
		}
		else if (m_iEpollSucc > 0){
			printf("###event count %d\n",m_iEpollSucc);
		}
		else {
			//printf("nothing to do socket event,m_nEvent:%d\n",m_iEvents);
		}

		return m_iEpollSucc;
	//}
	//else {
		//printf("nothing to do socket event,m_nEvent:%d\n",m_iEvents);
	//}
/*
	if (m_pUserEventHandler) {
		int iRet = m_pUserEventHandler->CheckEvent();

		if (iRet != 0) {
			printf("error happed when checkUserEvent %d\n",iRet);
			return 0;
		}

		return iRet;
	}*/

	//return m_iEvents | 1;

	return 0;
}

int CReactor::ProcessEvent() {
	/*
	 *
	typedef union epoll_data {
		void        *ptr;
		int          fd;
		uint32_t     u32;
		uint64_t     u64;
	} epoll_data_t;

	struct epoll_event {
		uint32_t     events;      // Epoll events--EPOLLIN(for read) EPOLLOUT(for write) 等等一堆事件的掩码值
		epoll_data_t data;        //User data variable
	};
	 */
	if (m_pUserEventHandler) {
		m_pUserEventHandler->OnEventFire();
	}

	for(int i = 0; i < m_iEpollSucc; ++i)
	{

		 //TODO 这里的UDP处理不完善
		stTcpSockItem* stItem =	reinterpret_cast<stTcpSockItem*>(m_arrEpollEvents[i].data.ptr);
		printf("###event from fd %d,eventFlag:%s\n",stItem->fd,GetEventFlag(stItem->enEventFlag));
		if ((m_arrEpollEvents[i].events & EPOLLRDHUP) == EPOLLRDHUP) { //客户端关闭
			if (stItem->enEventFlag != UDP_READ	|| stItem->enEventFlag != UDP_SEND) {
				int iRet = m_pTcpNetHandler->HandleEvent(stItem->fd,TCP_SERVER_CLOSE);
				if (0 != iRet) {
					return iRet;
				}
			}
		}
		else if (stItem->fd == m_iSvrFd) {
			int iRet = m_pTcpNetHandler->HandleEvent(m_iSvrFd, stItem->enEventFlag);
			if (0 != iRet) {
				return iRet;
			}
		}
		else if (stItem->enEventFlag == TCP_SERVER_READ) {
			int iRet = m_pTcpNetHandler->HandleEvent(stItem->fd, stItem->enEventFlag);
			if (0 != iRet ) {
				return iRet;
			}
		}
		else if (stItem->enEventFlag == UDP_READ) {
			int iRet = m_pUSockUdpHandler->HandleEvent(stItem->fd, stItem->enEventFlag);
			if (0 != iRet ) {
				return iRet;
			}
		}
		else if (stItem->enEventFlag == TCP_SERVER_SEND) {
			int iRet = m_pTcpNetHandler->HandleEvent(stItem->fd, stItem->enEventFlag);
			if (0 != iRet ) {
				return iRet;
			}
		}
		else  {
			printf("无效的请求不处理-丢弃\n");//socket事件不对
		}
		/* 传统只根据Fd进行处理的办法，无法适应复杂的代码编写结构
		 if (m_arrEpollEvents[i].data.fd == m_iSvrFd) {
			 int iRet = m_pTcpNetHandler->HandleEvent(m_iSvrFd,1);
			 if (0 != iRet ) {
				 return iRet;
			 }
		 }
		 else {
			 int iRet = m_pTcpNetHandler->HandleEvent(m_arrEpollEvents[i].data.fd,2);
			 if (0 != iRet ) {
			 	return iRet;
			 }
		 }*/

	}

	m_iEpollSucc = 0;
	return 0;
}

void CReactor::RunEventLoop() {
	int iRet = 0;
	if (m_iSvrFd) {
		AddToWatchList(m_iSvrFd, TCP_SERVER_ACCEPT, (void*)&m_arrTcpSock[m_iSvrFd], false); //将两大IO通道加入监听
	}
	if (m_iUSockFd) {
		AddToWatchList(m_iUSockFd, UDP_READ, (void*)&m_arrTcpSock[m_iUSockFd],false);
	}
	while (1) {
		iRet = this->CheckEvents();
		if (0 == iRet) { //没有任何东西要处理时，小小暂停一下
			/**
			 * 不用sleep是因为sleep会让整个进程切换状态，等待操作系统的唤醒调用，最差的情况可能要10s钟的时间
			 * 并不能精确的做到短时间段的sleep任务
			 *
			 * 不加等待机制（阻塞）的逻辑也是不对的，这样进程会占住cpu过多的时间才放开。不利于多进程间cpu时间的有效利用
			 */
			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = DEFAULT_EPOLL_WAIT_TIME;
			/*int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* errorfds, struct timeval* timeout);*/
			select(0,NULL,NULL,NULL,&tv); //纯纯的sleep
			//printf("just sleep a little\n");
		}

		iRet = this->ProcessEvent();
		if (iRet) {
			printf("something error happen!%d\n",iRet);
			break;
		}
	}
}

int CReactor::RegisterTcpNetHandler(CTcpNetHandler* pTcpNetHandler) {
	m_pTcpNetHandler = pTcpNetHandler;
	m_pTcpNetHandler->m_pReactor = this;
	return 0;
}

int CReactor::RegisterUSockUdpHandler(CUSockUdpHandler* pUSockUdpHandler) {
	m_pUSockUdpHandler = pUSockUdpHandler;
	m_pUSockUdpHandler->m_pReactor = this;
	return 0;
}

int CReactor::RegisterUserEventHandler(CUserEventHandler* pUserEventHandler) {
	m_pUserEventHandler = pUserEventHandler;
	m_pUserEventHandler->m_pReactor = this;
	return 0;
}

