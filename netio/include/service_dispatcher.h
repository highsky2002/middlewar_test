/*
 * service_dispatcher.h
 *
 *  Created on: 2016年3月1日
 *      Author: chenzhuo
 */

#ifndef NETIO_INCLUDE_SERVICE_DISPATCHER_H_
#define NETIO_INCLUDE_SERVICE_DISPATCHER_H_
#include <list>
#include "intf_service.h"
#include "cmd_obj.h"
using namespace std;
class CServiceDispatcher {
public:
	list<IService*> listStatefulSvcQueue;
	list<IService*> listStatelessSvcQueue;
	int m_iCmd;

	//队列认为是container关心的，分配器不拥有队列相关信息，交由container回写response到netio的队列
	int Dispatch(CCmd& oCmd);//调用IService对象的Execute函数

	int AddSvcHandler(IService*);
	CServiceDispatcher();
	~CServiceDispatcher();
};

#endif /* NETIO_INCLUDE_SERVICE_DISPATCHER_H_ */
