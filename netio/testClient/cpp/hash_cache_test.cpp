#include <stdio.h>
#include <iostream>
#include "mod_hash_v1.h"
#include "cache_manager.h"
#include <unistd.h>
using namespace std;

int main()
{
	//fork();
	CCacheManager* pcm = CCacheManager::GetInstance();
	//cm.Initalize();
	string key="c1_1";
	//std::cout<<pcm->Incr(key,17)<<std::endl;

	//std::cout<<pcm->Get(key,out)<<std::endl;
	//std::cout<<"read:"<<out<<endl;

	pcm->PrintAll();
	//delete pcm;
	return 0;
}
