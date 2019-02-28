/*
   Программа отлавливает пакеты нестандартной реализации trap'а
   в свитчах D-Link, поддерживающих управление через SmartConsole.

   This is free software and certainly "AS IS" and "NO WARRANTIES"

   Author: Alexander Melnik (olexander.v.melnyk@gmail.com), updates, patches and
   suggestions are welcomed.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <stdint.h>
#endif
#include <unistd.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <pcre.h>

#define BUFLEN 512
#define MAXPORTS 32
#define PIDFILEPATH "/var/run/"
#define STATEDIR "/var/db/state/"

// список поддерживаемых моделей свитчей,
// нужен для определения кол-ва портов для сохранения состояния.
// логирование trap'ов работает и без него.
char*	model_name[4]={
	"DES-1100-16",
	"DES-1100-24",
	"DES-2108",
//последняя запись должна быть пустой строкой
	""
};

// список кол-ва портов.
// номер элемента в этом массиве должен совпадать с
// номером модели в массиве model_name
int quantity_of_ports[]={
	16,	// DES-1100-16
	24,	// DES-1100-24
	 8	// DES-2108
};

extern int		errno;
int unsigned		debug=0;
int unsigned		savestate=0;
int unsigned		port=64514;
pcre			*re_main;
pcre			*re_port;
pcre_extra		*sd_main;
pcre_extra		*sd_port;
const char		*error;
int unsigned		portArray[MAXPORTS]={0};
int unsigned		qports=0;
uint8_t			mac[6];
const char		pattern_main[]="^(\\S+)\\s+\\((\\d+)\\)(.+)$";
const char		pattern_port[]="^\\S+\\s+(\\d+)\\s+\\S.+$";
char			progname[32];
char			pidfile[BUFLEN];
char			statedir[BUFLEN];
char			outfile[BUFLEN];
char			ip_remote[64];
char			buf[BUFLEN];
char			tmpstr[BUFLEN*2];

#ifdef __linux__
size_t strlcpy(char *dst, const char *src, size_t size){
 strncpy(dst,src,size);
 dst[size-1]='\0';
 return strlen(dst);
}

size_t strlcat(char *dst, const char *src, size_t size){
 strncat(dst,src,size);
 dst[size-1]='\0';
 return strlen(dst);
}
#endif

static void usage(void){
 fprintf(stderr, "Usage: %s [-d[d]] [-s] [-i <IP>] [-P <port>] [-p file] [-f dir]\n",progname);
 fprintf(stderr, "       d:               enable debug output\n");
 fprintf(stderr, "       s:               enable save ports state\n");
 fprintf(stderr, "       P <port>         listen port (default 64514)\n");
 fprintf(stderr, "       i <IP>:          listen address (default ANY)\n");
 fprintf(stderr, "       p <file>:        pid file\n");
 fprintf(stderr, "       f </some/dir/>:  directory for save ports state\n");
 exit(2);
}

static void print_dump(const char *buf,int len){
int i,j;
char s[17];
char c[2];

 j=0; printf ("0000: ");
 bzero(&s,sizeof(s));
 c[1]='\0';
 for(i=0;i<len;i++){
   printf("%02X ",(u_char)buf[i]);
   c[0]=(buf[i] < 32)?'.':(u_char)buf[i];
   if (strlen(s)) strlcat(s,c,sizeof(s));
   else strlcpy(s,c,sizeof(s));
   if (++j > 15) {j=0;printf("   %s\n%04X: ",s,i+1); bzero(&s,sizeof(s));}
 }
 if (strlen(s)) {
   for(i=0;i<(16-j);i++) printf("   ");
   printf ("   %s",s);
 }
 printf("\n");
 return;
}

void mylogger(int facility, char *err_string){
 if (debug) printf("Syslog record: \"%s\"\n",err_string);
 syslog(facility,"%s",err_string);
 return;
}

static void sigterm_handler(int signo) {
//char tmpstr[BUFLEN*2];

 if (!signo) {
   mylogger(LOG_ERR, "going down with fatal error");
 }else{
   bzero(&tmpstr,sizeof(tmpstr));
   snprintf(tmpstr,sizeof(tmpstr),"going down on signal \"%s\"",strsignal(signo));
   mylogger(LOG_ERR,tmpstr);
 }
 if (strlen(pidfile)) {
   if (unlink(pidfile)) {
     bzero(&tmpstr,sizeof(tmpstr));
     snprintf(tmpstr,sizeof(tmpstr),"error while deleting pid file %s: %s",pidfile,strerror(errno));
     mylogger(LOG_ERR,tmpstr);
   }
 }
 exit(1);
}

static void sigchld_handler(int signo) {
   while (waitpid(-1, NULL, WNOHANG) > 0);
}

int get_quantity_of_ports(char *model){
 int i=0;

 for(;;){
   if (!strlen(model_name[i])) return(0);
   if (!strcasecmp(model,model_name[i])) break;
   i++;
 }
 return(quantity_of_ports[i]);
}

// обновление файла с состояние портов свитча
void port_state_change(int msgCode, char *msg){
 int of,i,len,rc;
 int chport=255;
 struct flock fl;
 int ovector[12];
 char wrkfile[BUFLEN];
 char wrkstr[MAXPORTS*2+2];
 char chport_str[5];

 // формирование имени файла состояния
 bzero(&wrkfile,sizeof(wrkfile));
 snprintf(wrkfile,sizeof(wrkfile),"%s%02x:%02x:%02x:%02x:%02x:%02x",statedir,
          (unsigned int)mac[0],(unsigned int)mac[1],(unsigned int)mac[2],
          (unsigned int)mac[3],(unsigned int)mac[4],(unsigned int)mac[5]);

 // настройка флагов для блокировки доступа к файлу
 fl.l_type   = F_WRLCK;
 fl.l_whence = SEEK_SET;
 fl.l_pid    = getpid();
 fl.l_start  = 0;
 fl.l_len    = 0;
#ifdef __freebsd__
 fl.l_sysid  = 0;
#endif

 if (msgCode != 1001){
   // в случае изменения состояния порта, извлечь номер порта из сообщения свитча
   rc=pcre_exec(re_port, sd_port, msg, strlen(msg), 0, 0, ovector, 12);
   if (rc < -1){
     snprintf(tmpstr,sizeof(tmpstr),"pcre_exec error with code %i on string \"%s\"",rc,msg);
     mylogger(LOG_ERR,tmpstr);
     return;
   }
   if ( rc != 2 ){
     snprintf(tmpstr,sizeof(tmpstr),"incorrect message \"%s\" from switch %s (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
              msg,ip_remote,
              (unsigned int)mac[0],(unsigned int)mac[1],(unsigned int)mac[2],
              (unsigned int)mac[3],(unsigned int)mac[4],(unsigned int)mac[5]);
     mylogger(LOG_ERR,tmpstr);
     return;
   }
   bzero(&chport_str,sizeof(chport_str));
   (void)pcre_copy_substring(msg,ovector,rc,1,chport_str,sizeof(chport_str));
   chport=(int)strtol(chport_str,(char **)NULL, 10);
   if ( (chport < 1) || (chport > qports) ){
     snprintf(tmpstr,sizeof(tmpstr),"incorrect number of port %i from switch %s (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
              chport,ip_remote,
              (unsigned int)mac[0],(unsigned int)mac[1],(unsigned int)mac[2],
              (unsigned int)mac[3],(unsigned int)mac[4],(unsigned int)mac[5]);
     mylogger(LOG_ERR,tmpstr);
     return;
   }
 }

 // открытие файла на чтение+запись
 if ((of=open(wrkfile,O_RDWR|O_CREAT,0644)) < 0){
   snprintf(tmpstr,sizeof(tmpstr),"can't open file %s: %s",wrkfile,strerror(errno));
   mylogger(LOG_ERR,tmpstr);
   return;
 }
 // блокировка доступа к файлу
 if (fcntl(of, F_SETLKW, &fl) < 0) {
   snprintf(tmpstr,sizeof(tmpstr),"can't obtain exclusive lock for file %s: %s",wrkfile,strerror(errno));
   mylogger(LOG_ERR,tmpstr);
   return;
 }

 if (msgCode != 1001){
   // чтение исходного состояния портов при изменении
   bzero(&wrkstr,sizeof(wrkstr));
   len=qports*2-1;
   if ((rc=read(of,wrkstr,len)) == -1){
     snprintf(tmpstr,sizeof(tmpstr),"can't read from file %s: %s",wrkfile,strerror(errno));
     mylogger(LOG_ERR,tmpstr);
   }else{
     if (rc != len){
       snprintf(tmpstr,sizeof(tmpstr),"should be read %i bytes, but readed %i (file %s)",len,rc,wrkfile);
       mylogger(LOG_ERR,tmpstr);
     }else{
       // преобразование текстового представления из файла в массив
       for(i=0;i<qports;i++){
         switch (wrkstr[i*2]) {
           case '0':
                    break;
           case '1':
                    portArray[i]++;
                    break;
           default:
                    snprintf(tmpstr,sizeof(tmpstr),"incorrect symbol \"%c\" at position %i in file %s",wrkstr[i*2],i*2,wrkfile);
                    mylogger(LOG_ERR,tmpstr);
         }
       }
     }
     // вернуть указатель в начало файла для поледующей записи
     if (lseek(of,0,SEEK_SET) == -1){
       snprintf(tmpstr,sizeof(tmpstr),"can't reposition in file %s: %s",wrkfile,strerror(errno));
       mylogger(LOG_ERR,tmpstr);
     }
   }
   portArray[chport-1]=(msgCode == 3003)?1:0;
 }

 // преобразование массива портов в текстовый вид
 bzero(&wrkstr,sizeof(wrkstr));
 for (i=0;i<qports;i++)
   strlcat(wrkstr,(portArray[i])?"1,":"0,",sizeof(wrkstr));
 wrkstr[(len=strlen(wrkstr))-1]='\n';

 // запись файла
 if ((rc=write(of,wrkstr,len)) == -1){
   snprintf(tmpstr,sizeof(tmpstr),"can't write to file %s: %s",wrkfile,strerror(errno));
   mylogger(LOG_ERR,tmpstr);
 }else
   if (rc != len){
     snprintf(tmpstr,sizeof(tmpstr),"should be written %i bytes, but recorded %i (file %s)",len,rc,wrkfile);
     mylogger(LOG_ERR,tmpstr);
   }

 // снятие блокировки
 fl.l_type = F_UNLCK;
 if (fcntl(of, F_SETLK, &fl) == -1) {
   snprintf(tmpstr,sizeof(tmpstr),"can't unlock for file %s: %s",wrkfile,strerror(errno));
   mylogger(LOG_ERR,tmpstr);
 }

 if (close(of)) { // чиста поржать
   snprintf(tmpstr,sizeof(tmpstr),"I do not believe it: can't close the file %s: %s",wrkfile,strerror(errno));
   mylogger(LOG_ERR,tmpstr);
 }
 return;
}

// разборка полученного от свитча пакета
void switch_state (int pktsize){
 int ovector[30];
 int count=0;
 int msgCode=0;
 char msg[BUFLEN];
 char model[32];
 char msgCode_str[8];
 char msgClear[128];

 bzero(&msg,BUFLEN);
 bcopy(buf+4,mac,sizeof(mac));
 bcopy(buf+0x2c,msg,pktsize-0x2c);
 if (debug) {
   printf("\nRunning child with pid %d\nReceived packet from %s (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
          getpid(),ip_remote,
          (unsigned int)mac[0],(unsigned int)mac[1],(unsigned int)mac[2],
          (unsigned int)mac[3],(unsigned int)mac[4],(unsigned int)mac[5]);
   if (debug>1) print_dump(buf,pktsize);
   else printf("Data: %s\n",msg);
 }

 // разделение сообщения на составные части
 count=pcre_exec(re_main, sd_main, msg, strlen(msg), 0, 0, ovector, 30);
 if (count < -1){
   snprintf(tmpstr,sizeof(tmpstr),"pcre_exec error with code %i on string \"%s\"",count,msg);
   mylogger(LOG_ERR,tmpstr);
   errx(1,"%s\n",tmpstr);
 }
 if ( count != 4 ){
   snprintf(tmpstr,sizeof(tmpstr),"unknown or incorrect message \"%s\" from switch %s (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
            msg,ip_remote,
            (unsigned int)mac[0],(unsigned int)mac[1],(unsigned int)mac[2],
            (unsigned int)mac[3],(unsigned int)mac[4],(unsigned int)mac[5]);
   mylogger(LOG_ERR,tmpstr);
   errx(1,"%s\n",tmpstr);
 }

 bzero(&model,sizeof(model));
 bzero(&msgCode_str,sizeof(msgCode_str));
 bzero(&msgClear,sizeof(msgClear));
 // извлечение модели свитча
 (void)pcre_copy_substring(msg,ovector,count,1,model,sizeof(model));
 // извлечение кода сообщения
 (void)pcre_copy_substring(msg,ovector,count,2,msgCode_str,sizeof(msgCode_str));
 // извлечение собственно сообщения
 (void)pcre_copy_substring(msg,ovector,count,3,msgClear,sizeof(msgClear));
 // преобразование кода сообщения в числовой вид
 msgCode=(int)strtol(msgCode_str,(char **)NULL, 10);
 // определение кол-ва портов
 qports=get_quantity_of_ports(model);
 if (debug) printf ("Model: %s, %i ports, code of message: %i, clear message: %s\n",model,qports,msgCode,msgClear);

 if (!qports && savestate){
   snprintf(tmpstr,sizeof(tmpstr),"unknown quantity of ports, state saving disabled");
   mylogger(LOG_ERR,tmpstr);
   savestate=0;
 }

 // обработка сообщений
 if (savestate){
   switch (msgCode){
     case 1001:
     case 3003:
     case 3004:
		port_state_change(msgCode,msgClear);
		break;
     // сюда добавлять обработку новых сообщений
     default:
		break;
   }
 }
 // логирование сообщений
 snprintf(tmpstr,sizeof(tmpstr),"message from switch %s (MAC %02x:%02x:%02x:%02x:%02x:%02x): \"%s\"",
	  ip_remote,
	  (unsigned int)mac[0],(unsigned int)mac[1],(unsigned int)mac[2],
	  (unsigned int)mac[3],(unsigned int)mac[4],(unsigned int)mac[5],
	  msgClear);
 mylogger(LOG_ERR,tmpstr);
 exit(0);
}

int main(int argc, char *argv[]){
#ifndef __FreeBSD__
 char *pTemp;
#endif
 char ip_address[36];
 struct sockaddr_in siLocal, siRemote;
 int c,s,rc,pid;
 int erroffset;
 int unsigned slen=sizeof(siRemote);

 bzero(&progname,sizeof(progname));
 bzero(&ip_address,sizeof(ip_address));
 bzero(&pidfile,sizeof(pidfile));
 bzero(&statedir,sizeof(statedir));
 bzero(&tmpstr,sizeof(tmpstr));

#ifdef __FreeBSD__
 strlcpy(progname,getprogname(),sizeof(progname));
#else
 if ((pTemp=strrchr(argv[0],'/'))!=NULL) strlcpy(progname,pTemp+1,sizeof(progname));
 else strlcpy(progname,argv[0],sizeof(progname));
#endif

 openlog(progname,0,LOG_DAEMON);

 while ((c = getopt(argc, argv, "dsp:P:i:f:h?")) != -1) {
   switch (c) {
    case 'd':
              debug++;
              break;
    case 's':
              savestate++;
              break;
    case 'P':
              if (sscanf(optarg, "%u",&port) != 1) {
                snprintf(tmpstr,sizeof(tmpstr),"incorrect port \"%s\"",optarg);
                mylogger(LOG_ERR,tmpstr);
                errx(1,"%s\n",tmpstr);
              }
              break;
    case 'p':
              strlcpy(pidfile,optarg,sizeof(pidfile));
              break;
    case 'f':
              strlcpy(statedir,optarg,sizeof(statedir));
              if (strrchr(statedir,'/') != (statedir+strlen(statedir)-1))
                strlcat(statedir,"/",sizeof(statedir));
              break;
    case 'i':
              strlcpy(ip_address,optarg,sizeof(ip_address));
              break;
    default:
              usage();
              break;
   }
 }

 // открыть сокет
 if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1){
   snprintf(tmpstr,sizeof(tmpstr),"can't open socket: %s",strerror(errno));
   mylogger(LOG_ERR,tmpstr);
   errx(1,"%s\n",tmpstr);
 }

 // если не указан конкретный IP, биндится на все
 bzero(&siLocal, sizeof(siLocal));
 siLocal.sin_family = AF_INET;
 siLocal.sin_port = htons(port);
 if (strlen(ip_address)){
   if (inet_pton(AF_INET,ip_address,&siLocal.sin_addr) < 1){
     snprintf(tmpstr,sizeof(tmpstr),"incorrect IP address: %s",ip_address);
     mylogger(LOG_ERR,tmpstr);
     errx(1,"%s\n",tmpstr);
   }
 } else
   siLocal.sin_addr.s_addr = htonl(INADDR_ANY);
 if (bind(s, (struct sockaddr *)&siLocal, sizeof(siLocal))==-1){
   snprintf(tmpstr,sizeof(tmpstr),"can't bind: %s",strerror(errno));
   mylogger(LOG_ERR,tmpstr);
   errx(1,"%s\n",tmpstr);
 }

 // подготовка regexp'ов (главного)
 re_main=pcre_compile(pattern_main, 0, &error, &erroffset, NULL);
 if (re_main==NULL){
   snprintf(tmpstr,sizeof(tmpstr),"error compile pattern \"%s\": %s at byte %d",pattern_main, error, erroffset);
   mylogger(LOG_ERR,tmpstr);
   errx(1,"%s\n",tmpstr);
 }
 sd_main=pcre_study(re_main,0,&error);
 if (sd_main==NULL){
   snprintf(tmpstr,sizeof(tmpstr),"error study pattern \"%s\": %s",pattern_main, error);
   mylogger(LOG_ERR,tmpstr);
   errx(1,"%s\n",tmpstr);
 }
 if (savestate){
   // подготовка regexp'ов (номер порта)
   if (!strlen(statedir)) strlcpy(statedir,STATEDIR,sizeof(statedir));
   re_port=pcre_compile(pattern_port, 0, &error, &erroffset, NULL);
   if (re_port==NULL){
     snprintf(tmpstr,sizeof(tmpstr),"error compile pattern \"%s\": %s at byte %d",pattern_port, error, erroffset);
     mylogger(LOG_ERR,tmpstr);
     errx(1,"%s\n",tmpstr);
   }
   sd_port=pcre_study(re_port,0,&error);
   if (sd_port==NULL){
     snprintf(tmpstr,sizeof(tmpstr),"error study pattern \"%s\": %s",pattern_port, error);
     mylogger(LOG_ERR,tmpstr);
     errx(1,"%s\n",tmpstr);
   }
 }

 if (!debug) {
   // при сохранении состояния портов сделать текущим каталог с файлами состояния
   if (savestate){
     if (chdir(statedir)){
        snprintf(tmpstr,sizeof(tmpstr),"couldn't chdir to directory \"%s\": %s", statedir, strerror(errno));
        mylogger(LOG_ERR,tmpstr);
        fprintf(stderr, "%s\n",tmpstr);
     }
   }
   // демонизация
   if (daemon((savestate)?0:1,1)){
      snprintf(tmpstr,sizeof(tmpstr),"couldn't daemonize: %s", strerror(errno));
      mylogger(LOG_ERR,tmpstr);
   }
   // запись pid-файла
   if (strlen(pidfile) < 1)
     snprintf(pidfile,sizeof(pidfile),"%s%s.pid",PIDFILEPATH,progname);
   FILE *f = fopen(pidfile, "w");
   if (f == NULL) {
      snprintf(tmpstr,sizeof(tmpstr),"couldn't create pid file \"%s\": %s", pidfile, strerror(errno));
      mylogger(LOG_ERR,tmpstr);
      bzero(&pidfile,sizeof(pidfile));
  }else{
      fprintf(f, "%ld\n", (long) getpid());
      fclose(f);
   }
 }

 // перeхват сигналов
 signal(SIGCHLD, sigchld_handler);
 signal(SIGTERM, sigterm_handler);
 signal(SIGQUIT, sigterm_handler);
 signal(SIGINT,  sigterm_handler);

 // Run, Lola, run...
 for (;;) {
   bzero(&buf,BUFLEN);
   rc=recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *)&siRemote, &slen);
   if (rc == -1){
     snprintf(tmpstr,sizeof(tmpstr),"recvfrom: %s",strerror(errno));
     mylogger(LOG_ERR,tmpstr);
   }
   else if (rc) {
     pid=fork();
     switch (pid) {
       case 0:
         bzero(&ip_remote,sizeof(ip_remote));
         strlcpy(ip_remote,inet_ntoa(siRemote.sin_addr),sizeof(ip_remote));
         switch_state(rc);
         exit(0);
         break;
       case -1:
         snprintf(tmpstr,sizeof(tmpstr),"fork error: %s",strerror(errno));
         mylogger(LOG_ERR, tmpstr);
         break;
       default:
         break;
     }
   }
 }
 exit(0);
}
