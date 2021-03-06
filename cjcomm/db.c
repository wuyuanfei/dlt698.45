/*
 * db.c
 *
 *  Created on: 2017-7-26
 *      Author: zhoulihai
 */

#include "db.h"
#include "basedef.h"

static DBStruct DB;

void dbInit(int index) {
	readCoverClass(0x4521, 0, (void *) &DB.model_2g, sizeof(DB.model_2g),
			para_vari_save);
//	if (DB.model_2g == 666) {
//		asyslog(LOG_INFO, "警告，现在是强制2G上线模式....");
//	}
	readCoverClass(0x4500, 0, (void *) &DB.c25, sizeof(DB.c25), para_vari_save);
	asyslog(LOG_INFO, "连接应用方式 enum{主备模式(0),多连接模式(1)}：%d",
			DB.c25.commconfig.appConnectType);
	asyslog(LOG_INFO, "超时时间，重发次数：%02x", DB.c25.commconfig.timeoutRtry);
	asyslog(LOG_INFO, "心跳周期秒：%d", DB.c25.commconfig.heartBeat);
	readCoverClass(0xf101, 0, (void *) &DB.gprs.f101, sizeof(CLASS_F101),
			para_vari_save);

	readCoverClass(0x4510, 0, (void *) &DB.c26, sizeof(DB.c26), para_vari_save);
	asyslog(LOG_INFO, "连接应用方式 enum{主备模式(0),多连接模式(1)}：%d",
			DB.c26.commconfig.appConnectType);
	asyslog(LOG_INFO, "超时时间，重发次数：%02x", DB.c26.commconfig.timeoutRtry);
	asyslog(LOG_INFO, "心跳周期秒：%d", DB.c26.commconfig.heartBeat);
	readCoverClass(0xf101, 0, (void *) &DB.net.f101, sizeof(CLASS_F101),
			para_vari_save);

	readCoverClass(0xf202, 0, &DB.cf202, sizeof(DB.cf202), para_vari_save);

	DB.JProgramInfo = OpenShMem("ProgramInfo", sizeof(ProgramInfo), NULL);

	if (DB.JProgramInfo->cfg_para.device == CCTT1
			|| DB.JProgramInfo->cfg_para.device == SPTF3) {
		CLASS_f201 f201[3];
		memset(&f201,0,sizeof(f201));
		readCoverClass(0xf201, 0, &f201, sizeof(f201), para_vari_save);

		DB.RS485IIOPEN = (f201[1].devfunc == 0) ? 1 : 0;
		memcpy(&DB.cf200, &f201[1], sizeof(DB.cf200));

		//这里没错！这里是为了让默认维护口的参数都存在DB.cf201里
		readCoverClass(0xf200, 0, &DB.cf201, sizeof(DB.cf201), para_vari_save);
	}
	if (DB.JProgramInfo->cfg_para.device == CCTT2) {
		CLASS_f201 f201[3];
		memset(&f201,0,sizeof(f201));
		readCoverClass(0xf201, 0, &f201, sizeof(f201), para_vari_save);
		memcpy(&DB.cf201, &f201[2], sizeof(DB.cf201));
		DB.RS485IIOPEN = 0;
	}

	initComPara(&DB.ifr, cWrite);
	initComPara(&DB.serial, cWrite);
	initComPara(&DB.serial_hn, cWrite);
	initComPara(&DB.net, cWrite);
	initComPara(&DB.gprs, cWriteWithCalc);

	DB.ifr.name = 1;
	DB.serial.name = 2;
	DB.serial_hn.name = 5;
	DB.net.name = 3;
	DB.gprs.name = 4;

	DB.last_proxy = &DB.serial;

	DB.gprs.Heartbeat = DB.c25.commconfig.heartBeat;
	DB.net.Heartbeat = DB.c26.commconfig.heartBeat;
	memcpy(DB.JProgramInfo->Projects[index].ProjectName, "cjcomm",
			sizeof("cjcomm"));

	memset(&DB.JProgramInfo->dev_info.realTimeC2200, 0x00, sizeof(Flow_tj));
	readVariData(0x2200, 0, &DB.JProgramInfo->dev_info.realTimeC2200,
			sizeof(Flow_tj));
	asyslog(LOG_INFO, "初始化月流量统计(%d)",
			DB.JProgramInfo->dev_info.realTimeC2200.flow.month_tj);

	DB.JProgramInfo->Projects[index].ProjectID = getpid();
	readCoverClass(0x6099, 0, &DB.taskList, sizeof(taskFailInfo_s), para_vari_save);
	asyslog(LOG_INFO, "初始化任务补报(%02x-%02x)(%02x-%02x)(%02x-%02x)",
	                        DB.taskList.rptList[0][0],DB.taskList.rptList[0][1],
	                        DB.taskList.rptList[1][0],DB.taskList.rptList[1][1],
	                        DB.taskList.rptList[2][0],DB.taskList.rptList[2][1]);

	DB.OnlineType = 0;
	DB.ProgIndex = index;
	DB.CalcNew = 0;
	DB.GprsType = 1;
	DB.RS485IIIAutoReport = 0;
	DB.StopCommunite = 0;
}

void * dbGet(char * name) {
	if (strcmp("StopCommunite", name) == 0) {
		return DB.StopCommunite;
	}

	if (strcmp("proxy", name) == 0) {
		return DB.last_proxy;
	}

	if (strcmp("485auto", name) == 0) {
		return DB.RS485IIIAutoReport;
	}
	if (strcmp("block.ifr", name) == 0) {
		return &DB.ifr;
	}
	if (strcmp("block.net", name) == 0) {
		return &DB.net;
	}
	if (strcmp("block.gprs", name) == 0) {
		return &DB.gprs;
	}
	if (strcmp("block.serial", name) == 0) {
		return &DB.serial;
	}
	if (strcmp("block.serial_hn", name) == 0) {
		return &DB.serial_hn;
	}
	if (strcmp("class25", name) == 0) {
		return &DB.c25;
	}
	if (strcmp("class26", name) == 0) {
		return &DB.c26;
	}
	if (strcmp("f201", name) == 0) {
		return &DB.cf201;
	}
	if (strcmp("f202", name) == 0) {
		return &DB.cf202;
	}
	if (strcmp("program.info", name) == 0) {
		return DB.JProgramInfo;
	}
	if (strcmp("prog.index", name) == 0) {
		return DB.ProgIndex;
	}
	if (strcmp("online.type", name) == 0) {
		return DB.OnlineType;
	}
	if (strcmp("calc.new", name) == 0) {
		return DB.CalcNew;
	}
	if (strcmp("gprs.type", name) == 0) {
		return DB.GprsType;
	}
	if (strcmp("mmq.retry_buf", name) == 0) {
		return &DB.retry_buf;
	}
	if (strcmp("mmq.retry_head", name) == 0) {
		return &DB.retry_head;
	}
	if (strcmp("mmq.retry_count", name) == 0) {
		return DB.retry_count;
	}
	if (strcmp("model_2g", name) == 0) {
		return DB.model_2g;
	}
	if (strcmp("4852open", name) == 0) {
		return DB.RS485IIOPEN;
	}
	if (strcmp("f200", name) == 0) {
		return &DB.cf200;
	}

	if (strcmp("task_list", name) == 0) {
	        return &DB.taskList;
	}
	if (strcmp("curr_task", name) == 0) {
	        return &DB.currTask;
	}
	if (strcmp("curr_retry_task", name) == 0) {
	        return &DB.currRetryTask;
	}
	return (void *) 0;
}

int dbSet(char * name, void* data) {

	if (strcmp("StopCommunite", name) == 0) {
		DB.StopCommunite = (int) data;
	}
	if (strcmp("485auto", name) == 0) {
		DB.RS485IIIAutoReport = (int) data;
	}
	if (strcmp("online.type", name) == 0) {
		DB.OnlineType = (int) data;
	}
	if (strcmp("calc.new", name) == 0) {
		DB.CalcNew = (int) data;
	}
	if (strcmp("gprs.type", name) == 0) {
		DB.GprsType = (int) data;
	}

	if (strcmp("proxy", name) == 0) {
		DB.last_proxy = (CommBlock*)data;
	}
	if (strcmp("mmq.retry_count", name) == 0) {
		DB.retry_count = (int) data;
	}

	if(strcmp("model_2g", name) == 0) {
		switch ((int)data) {
			case 0x11:
			case 0x22:
			case 0x33:
			case 0x44:
			case 0x55:
				DB.model_2g = (int) data;
                break;
			default:
				DB.model_2g = 0x00;
		}
	}
	return 1;
}
