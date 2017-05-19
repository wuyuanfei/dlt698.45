#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <pthread.h>

#include "PublicFunction.h"
#include "dlt698def.h"
#include "cjcomm.h"
#include "../include/Shmem.h"
#include "clientOnModel.h"

/*
 * 内部协议栈参数
 */
static CLASS25 Class25;
static CommBlock ClientForModelObject;
static long long ClientOnModel_Task_Id;
static MASTER_STATION_INFO NetIps[4];
static pthread_mutex_t locker;
static NetObject netObject;

static int NetSend(int fd, INT8U *buf, INT16U len) {
    if (len > 2048) {
        asyslog(LOG_WARNING, "发送报文太长[2048-%d]", len);
        return 0;
    }
    pthread_mutex_lock(&locker);
    if (netObject.tail + 1 != netObject.head) {
        memcpy(netObject.send[netObject.tail].buf, buf, len);
        netObject.send[netObject.tail].len = len;
        asyslog(LOG_WARNING, "内部协议栈发送(长度:%d,头尾:%d-%d)", len, netObject.head, netObject.tail);
        netObject.tail++;
        netObject.tail %= 16;
    } else {
        asyslog(LOG_WARNING, "内部协议栈发送[缓冲区满](长度:%d,头尾:%d-%d)", len, netObject.head, netObject.tail);
    }
    pthread_mutex_unlock(&locker);
}

static int NetRecv(INT8U *buf) {
    pthread_mutex_lock(&locker);
    int len = netObject.recv.len;
    if (len < 1) {
        pthread_mutex_unlock(&locker);
        return 0;
    }
    netObject.recv.len = 0;
    for (int i = 0; i < len; ++i) {
        buf[i] = netObject.recv.buf[i];
    }
    pthread_mutex_unlock(&locker);
    return len;
}

static int getNext(INT8U *buf) {
    if (netObject.head == netObject.tail) {
        return -1;
    }
    pthread_mutex_lock(&locker);
    int res = netObject.head;
    int len = netObject.send[res].len;
    for (int i = 0; i < netObject.send[res].len; ++i) {
        buf[i] = netObject.send[res].buf[i];
    }
    netObject.head++;
    netObject.head %= 16;
    pthread_mutex_unlock(&locker);
    return len;
}

static int putNext(INT8U *buf, INT16U len) {
    pthread_mutex_lock(&locker);
    if (netObject.recv.len + len > 2048) {
        pthread_mutex_unlock(&locker);
        return -1;
    }

    for (int i = 0; i < len; ++i) {
        netObject.recv.buf[netObject.recv.len + i] = buf[i];
        printf("%02x  ", buf[i]);
    }
    printf("\n");
    netObject.recv.len += len;
    pthread_mutex_unlock(&locker);
    return len;
}

static MASTER_STATION_INFO getNextGprsIpPort(CommBlock *commBlock) {
    static int index = 0;
    static int ChangeFlag = -1;
    //检查主站参数是否有变化
    if (ChangeFlag != ((ProgramInfo *) commBlock->shmem)->oi_changed.oi4500) {
        readCoverClass(0x4500, 0, (void *) &Class25, sizeof(CLASS25), para_vari_save);
        memcpy(&NetIps, &Class25.master.master, sizeof(NetIps));
        asyslog(LOG_WARNING, "检测到通信参数变化！刷新主站参数！");
        ChangeFlag = ((ProgramInfo *) commBlock->shmem)->oi_changed.oi4500;
        commBlock->Heartbeat = Class25.commconfig.heartBeat;
        readCoverClass(0xf101, 0, (void *) &ClientForModelObject.f101, sizeof(CLASS_F101), para_vari_save);
    }

    MASTER_STATION_INFO res;
    memset(&res, 0x00, sizeof(MASTER_STATION_INFO));
    snprintf((char *) res.ip, sizeof(res.ip), "%d.%d.%d.%d", NetIps[index].ip[1], NetIps[index].ip[2],
             NetIps[index].ip[3], NetIps[index].ip[4]);
    res.port = NetIps[index].port;
    index++;
    index %= 2;
    asyslog(LOG_INFO, "客户端[GPRS]尝试链接的IP地址：%s:%d", res.ip, res.port);
    return res;
}

int SendCommandGetOK(int fd, int retry, const char *fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    char cmd[128];
    memset(cmd, 0x00, sizeof(cmd));
    vsprintf(cmd, fmt, argp);
    va_end(argp);

    for (int timeout = 0; timeout < retry; timeout++) {
        char Mrecvbuf[128];
        SendATCommand(cmd, strlen(cmd), fd);
        delay(800);
        memset(Mrecvbuf, 0, 128);
        RecieveFromComm(Mrecvbuf, 128, fd);
        if (strstr(Mrecvbuf, "OK") != 0) {
            return 1;
        }
    }
    return 0;
}

void resetModel() {
    asyslog(LOG_INFO, "重置模块状态...");
    gpofun("/dev/gpoGPRS_POWER", 0);
    sleep(8);
    gpofun("/dev/gpoGPRS_POWER", 1);
    gpofun("/dev/gpoGPRS_RST", 1);
    gpofun("/dev/gpoGPRS_SWITCH", 1);
    sleep(2);
    gpofun("/dev/gpoGPRS_RST", 0);
    sleep(1);
    gpofun("/dev/gpoGPRS_RST", 1);
    sleep(5);
    gpofun("/dev/gpoGPRS_SWITCH", 0);
    sleep(1);
    gpofun("/dev/gpoGPRS_SWITCH", 1);
    sleep(10);
}

int getNetType(int fd) {
    char Mrecvbuf[128];
    for (int timeout = 0; timeout < 10; timeout++) {
        SendATCommand("\rAT$MYTYPE?\r", 12, fd);
        delay(1000);
        memset(Mrecvbuf, 0, 128);
        RecieveFromComm(Mrecvbuf, 128, fd);

        int k, l, m;
        if (sscanf(Mrecvbuf, "%*[^:]: %d,%d,%d", &k, &l, &m) == 3) {
            if ((l & 0x01) == 1) {
                asyslog(LOG_INFO, "远程通信单元类型为GPRS。\n");
                break;
            }
            if ((l & 0x08) == 8) {
                asyslog(LOG_INFO, "远程通信单元类型为CDMA2000。\n");
                break;
            }
        }
    }
}

int getCIMIType(int fd) {
    char *cimiType[] = {
            "46003", "46011",
    };

    char Mrecvbuf[128];
    for (int timeout = 0; timeout < 5; timeout++) {
        SendATCommand("\rAT+CIMI\r", 9, fd);
        delay(1000);
        memset(Mrecvbuf, 0, 128);
        RecieveFromComm(Mrecvbuf, 128, fd);

        char cimi[64];
        memset(cimi, 0x00, sizeof(cimi));
        if (sscanf((char *) &Mrecvbuf[0], "%*[^0-9]%[0-9]", cimi) == 1) {
            asyslog(LOG_INFO, "CIMI = %s\n", cimi);
            for (int i = 0; i < 2; i++) {
                if (strncmp(cimiType[i], cimi, 5) == 0) {
                    return 3;
                }
            }
            return 1;
        }
    }
    return 0;
}

int regIntoNet(int fd) {
    char Mrecvbuf[128];
    for (int timeout = 0; timeout < 80; timeout++) {
        SendATCommand("\rAT+CREG?\r", 10, fd);
        delay(1000);
        memset(Mrecvbuf, 0, 128);
        RecieveFromComm(Mrecvbuf, 128, fd);

        int k, l;
        if (sscanf(Mrecvbuf, "%*[^:]: %d,%d", &k, &l) == 2) {
            asyslog(LOG_INFO, "GprsCREG = %d,%d\n", k, l);
            if (l == 1 || l == 5) {
                return 1;
            }
        }
    }
    return 0;
}

void *ModelWorker(void *args) {
    CLASS25 *class25 = (CLASS25 *) args;
    int sMux0 = -1;

    while (1) {
        gpofun("/dev/gpoCSQ_GREEN", 0);
        gpofun("/dev/gpoCSQ_RED", 0);
        gpofun("/dev/gpoONLINE_LED", 0);

        resetModel();

        if (GetOnlineType() != 0) { goto wait; }
        if ((sMux0 = OpenCom(5, 115200, (unsigned char *) "none", 1, 8)) < 0) { goto err; }
        if (SendCommandGetOK(sMux0, 5, "\rat\r") == 0) { goto err; }

        ////////////////////获取信息////////////////////
        for (int timeout = 0; timeout < 10; timeout++) {
            char Mrecvbuf[128];

            SendATCommand("\rAT$MYGMR\r", 10, sMux0);
            delay(1000);
            memset(Mrecvbuf, 0, 128);
            RecieveFromComm(Mrecvbuf, 128, sMux0);

            char INFO[6][32];
            if (sscanf(Mrecvbuf, "%*[^\n]\n%[^\n]\n%[^\n]\n%[^\n]\n%[^\n]\n%[^\n]\n%[^\n]", INFO[0], INFO[1], INFO[2],
                       INFO[3], INFO[4], INFO[5]) == 6) {
                break;
            }
        }

        for (int timeout = 0; timeout < 10; timeout++) {
            char Mrecvbuf[128];

            SendATCommand("\rAT$MYCCID\r", 11, sMux0);
            delay(1000);
            memset(Mrecvbuf, 0, 128);
            RecieveFromComm(Mrecvbuf, 128, sMux0);
            char CCID[32];
            memset(CCID, 0, 32);
            if (sscanf(Mrecvbuf, "%*[^\"]\"%[0-9|A-Z|a-z]", CCID) == 1) {
                asyslog(LOG_INFO, "CCID: %s\n", CCID);
                memcpy(class25->ccid, CCID, sizeof(32));
                break;
            }
        }

        for (int timeout = 0; timeout < 50; timeout++) {
            char Mrecvbuf[128];

            SendATCommand("\rAT+CSQ\r", 8, sMux0);
            delay(1000);
            memset(Mrecvbuf, 0, 128);
            RecieveFromComm(Mrecvbuf, 128, sMux0);

            int k, l;
            if (sscanf(Mrecvbuf, "%*[^:]: %d,%d", &k, &l) == 2) {
                asyslog(LOG_INFO, "GprsCSQ = %d,%d\n", k, l);
                if (k != 99) {
                    class25->signalStrength = k;
                    if (k > 20) {
                        gpofun("/dev/gpoCSQ_GREEN", 1);
                    } else if (k > 10) {
                        gpofun("/dev/gpoCSQ_GREEN", 1);
                        gpofun("/dev/gpoCSQ_RED", 1);
                    } else {
                        gpofun("/dev/gpoCSQ_RED", 1);
                    }
                    break;
                }
            }
        }
        saveCoverClass(0x4500, 0, class25, sizeof(CLASS25), para_vari_save);
        ////////////////////获取信息////////////////////


        if (GetOnlineType() != 0) { goto wait; }
        if (regIntoNet(sMux0) == 0) { goto err; }

        switch (getCIMIType(sMux0)) {
            case 1:
                SendCommandGetOK(sMux0, 5, "\rAT$MYNETACT=1,0\r");
                if (SendCommandGetOK(sMux0, 5, "\rAT$MYNETCON=1,\"APN\",\"%s\"\r", "cmnet") == 0) { goto err; }
                if (SendCommandGetOK(sMux0, 5, "\rAT$MYNETCON=1,\"USERPWD\",\"None,None\"\r") == 0) { goto err; }
                if (SendCommandGetOK(sMux0, 5, "\rAT$MYNETURC=1\r") == 0) { goto err; }
                MASTER_STATION_INFO m = getNextGprsIpPort(&ClientForModelObject);
                if (SendCommandGetOK(sMux0, 5, "\rAT$MYNETSRV=1,1,0,0,\"%s:%d\"\r", m.ip, m.port) == 0) { goto err; }
                if (SendCommandGetOK(sMux0, 8, "\rAT$MYNETACT=1,1\r") == 0) { goto err; }
                if (SendCommandGetOK(sMux0, 20, "\rAT$MYNETOPEN=1\r") == 0) { goto err; }
                SetOnlineType(3);
                break;
            case 3:
                break;
            default:
                goto err;
        }

        wait:
        //等待在线状态为“否”，重新拨号
        while (1) {
            sleep(1);
            if (GetOnlineType() == 0) {
                goto err;
            }

            INT8U sendBuf[2048];
            memset(sendBuf, 0x00, sizeof(sendBuf));
            int readySendLen = getNext(sendBuf);

            if (readySendLen != -1) {
                for (int timeout = 0; timeout < 3; timeout++) {
                    char Mrecvbuf[128];

                    char CommandBuf[128];
                    memset(CommandBuf, 0x00, sizeof(CommandBuf));
                    sprintf(CommandBuf, "\rAT$MYNETWRITE=1,%d\r", readySendLen);

                    SendATCommand(CommandBuf, strlen(CommandBuf), sMux0);
                    write(sMux0, CommandBuf, strlen(CommandBuf));
                    delay(3000);
                    memset(Mrecvbuf, 0, 128);
                    RecieveFromComm(Mrecvbuf, 128, sMux0);

                    int k = 0;
                    int l = 0;

                    if (sscanf(Mrecvbuf, "%*[^:]: %d,%d", &k, &l) == 2) {

                        SendATCommand(sendBuf, res, sMux0);
                        for (int j = 0; j < 3; ++j) {
                            delay(1000);
                            memset(Mrecvbuf, 0, 128);
                            RecieveFromComm(Mrecvbuf, 128, sMux0);
                        }
                        break;
                    }
                }
            }

            for (int timeout = 0; timeout < 3; timeout++) {
                char Mrecvbuf[128];

                char CommandBuf[128];
                memset(CommandBuf, 0x00, sizeof(CommandBuf));
                sprintf(CommandBuf, "\rAT$MYNETREAD=1,1024\r");

                SendATCommand("\rAT$MYNETREAD=1,1024\r", strlen("\rAT$MYNETREAD=1,1024\r"), sMux0);
                delay(1000);
                memset(Mrecvbuf, 0, 128);
                RecieveFromComm(Mrecvbuf, 128, sMux0);

                int k = 0;
                int l = 0;

                if (sscanf(Mrecvbuf, "%*[^:]: %d,%d", &k, &l) == 2) {
                    printf("============%d-%d\n", k, l);
                    if (l == 0) {
                        break;
                    }
                    for (int i = 0; i < l; ++i) {
                        printf("%02x ", Mrecvbuf[40 + i]);
                    }
                    printf("\n");
                    putNext(&Mrecvbuf[40], l);
                    break;
                } else {
                    printf("+++++++++++++++++++++++");
                }
            }

        }

        err:
        sleep(1);
        close(sMux0);
        continue;
    }

    return NULL;
}

void CreateOnModel(void *clientdata) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_mutex_init(&locker, NULL);

    pthread_t temp_key;
    pthread_create(&temp_key, &attr, ModelWorker, clientdata);
}

static int RegularClientOnModel(struct aeEventLoop *ep, long long id, void *clientData) {
    CommBlock *nst = (CommBlock *) clientData;

    if (GetOnlineType() != 3) {
        return 1000;
    }


    INT8U recvBuf[2048];
    memset(recvBuf, 0x00, sizeof(recvBuf));

    int revcount = NetRecv(recvBuf);
    if (revcount > 0) {
        for (int j = 0; j < revcount; j++) {
            nst->RecBuf[nst->RHead] = recvBuf[j];

            nst->RHead = (nst->RHead + 1) % BUFLEN;
        }

        bufsyslog(nst->RecBuf, "客户端[GPRS]接收:", nst->RHead, nst->RTail, BUFLEN);

        for (int k = 0; k < 5; k++) {
            int len = 0;
            for (int i = 0; i < 5; i++) {
                len = StateProcess(nst, 10);
                if (len > 0) {
                    break;
                }
            }
            if (len <= 0) {
                break;
            }

            if (len > 0) {
                int apduType = ProcessData(nst);
                fprintf(stderr, "apduType=%d\n", apduType);
                ConformAutoTask(ep, nst, apduType);
                switch (apduType) {
                    case LINK_RESPONSE:
                        First_VerifiTime(nst->linkResponse, nst->shmem); //简单对时
                        if (GetTimeOffsetFlag() == 1) {
                            Getk_curr(nst->linkResponse, nst->shmem);
                        }
                        nst->linkstate = build_connection;
                        nst->testcounter = 0;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    Comm_task(nst);
    check_F101_changed_Gprs(nst);
    CalculateTransFlow(nst->shmem);
    //暂时忽略函数返回
    RegularAutoTask(ep, nst);

    return 1000;
}

/*
 * 模块*内部*使用的初始化参数
 */
static void ClientOnModelInit(void) {
    asyslog(LOG_INFO, "\n\n>>>======初始化（内部协议栈[GPRS]模式）模块======<<<");

    //读取设备参数
    readCoverClass(0x4500, 0, (void *) &Class25, sizeof(CLASS25), para_vari_save);
    asyslog(LOG_INFO, "工作模式 enum{混合模式(0),客户机模式(1),服务器模式(2)}：%d", Class25.commconfig.workModel);
    asyslog(LOG_INFO, "在线方式 enum{永久在线(0),被动激活(1)}：%d", Class25.commconfig.onlineType);
    asyslog(LOG_INFO, "连接方式 enum{TCP(0),UDP(1)}：%d", Class25.commconfig.connectType);
    asyslog(LOG_INFO, "连接应用方式 enum{主备模式(0),多连接模式(1)}：%d", Class25.commconfig.appConnectType);
    asyslog(LOG_INFO, "侦听端口列表：%d", Class25.commconfig.listenPort[0]);
    asyslog(LOG_INFO, "超时时间，重发次数：%02x", Class25.commconfig.timeoutRtry);
    asyslog(LOG_INFO, "心跳周期秒：%d", Class25.commconfig.heartBeat);
    memcpy(&NetIps, &Class25.master.master, sizeof(NetIps));
    asyslog(LOG_INFO, "主站通信地址(1)为：%d.%d.%d.%d:%d", NetIps[0].ip[1], NetIps[0].ip[2], NetIps[0].ip[3],
            NetIps[0].ip[4],
            NetIps[0].port);
    asyslog(LOG_INFO, "主站通信地址(2)为：%d.%d.%d.%d:%d", NetIps[1].ip[1], NetIps[1].ip[2], NetIps[1].ip[3],
            NetIps[1].ip[4],
            NetIps[1].port);

    initComPara(&ClientForModelObject, NetSend);
    ClientForModelObject.Heartbeat = Class25.commconfig.heartBeat;
    readCoverClass(0xf101, 0, (void *) &ClientForModelObject.f101, sizeof(CLASS_F101), para_vari_save);

    asyslog(LOG_INFO, ">>>======初始化（内部协议栈[GPRS]模式）结束======<<<");
}


/*
 * 供外部使用的初始化函数，并开启维护循环
 */
int StartClientOnModel(struct aeEventLoop *ep, long long id, void *clientData) {
    ClientOnModelInit();
    CreateOnModel(&Class25);
    ClientOnModel_Task_Id = aeCreateTimeEvent(ep, 1000, RegularClientOnModel, &ClientForModelObject, NULL);
    asyslog(LOG_INFO, "内部协议栈[GPRS]时间事件注册完成(%lld)", ClientOnModel_Task_Id);

    return 1;
}

/*
 * 用于程序退出时调用
 */
void ClientOnModelDestory(void) {
    asyslog(LOG_INFO, "开始关闭内部协议栈[GPRS]接口(%d)", ClientForModelObject.phy_connect_fd);
    close(ClientForModelObject.phy_connect_fd);
    ClientForModelObject.phy_connect_fd = -1;
}