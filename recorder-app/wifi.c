#define _GNU_SOURCE
#include "wifi.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CONFIG "/data/cfg/wpa_supplicant.conf"
#define CONTROL "/var/run/wpa_supplicant/wlan0"
#define OFF_FILE "/userdata/e1-recorder/wifi-disabled"
#define MAX_NETS 128
/* Only this worker touches the control sockets and network configuration. */
enum { JOB_NONE, JOB_SCAN, JOB_CONNECT, JOB_FORGET, JOB_ENABLE, JOB_CANCEL };
typedef struct { int job, enabled, id; char hex[65], password[65], security[12]; } Job;
typedef struct { char ssid[129], hex[65], security[12]; int id, signal, supported; } Network;
static struct { pthread_mutex_t mutex; pthread_t thread; int running, started; Job next; char *json; } W = { .mutex=PTHREAD_MUTEX_INITIALIZER };
static int ctrl=-1, monitor=-1, enabled=1, available, scanning, candidate=-1, target=-1, replaced=-1;
static char ctrl_path[100], monitor_path[100], current_ssid[129], address[32], state[48]="Starting", message[160];
static Network nets[MAX_NETS];static int net_count;
static unsigned long long scan_until, connect_until;
static int connecting;
static unsigned long long now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static void wipe(void *p,size_t n) {volatile unsigned char *s=p;while(n--)*s++=0;}
static void json_string(FILE *f,const char *s) {
    fputc('"',f);for(const unsigned char *p=(const unsigned char *)s;*p;p++) {
        if(*p=='"'||*p=='\\')fprintf(f,"\\%c",*p);else if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
    }fputc('"',f);
}
static int unhex(char c) {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int decode_hex(const char *hex,unsigned char *raw) {
    size_t n=strlen(hex);if(n<2||n>64||n%2)return -1;
    for(size_t i=0;i<n;i+=2){int a=unhex(hex[i]),b=unhex(hex[i+1]);if(a<0||b<0)return -1;raw[i/2]=(a<<4)|b;}return n/2;
}
/* wpa_supplicant prints SSIDs using printf_encode, not shell quoting. */
static void decode_ssid(const char *in,Network *n) {
    unsigned char raw[33];int count=0;
    while(*in&&count<32) {
        unsigned char c=*in++;
        if(c=='\\'&&*in) {
            c=*in++;
            if(c=='x'&&in[0]&&in[1]&&unhex(in[0])>=0&&unhex(in[1])>=0){c=unhex(in[0])*16+unhex(in[1]);in+=2;}
            else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='e')c=27;
        }
        raw[count++]=c;
    }
    for(int i=0;i<count;i++)sprintf(n->hex+2*i,"%02x",raw[i]);n->hex[2*count]=0;
    /* Retain UTF-8 names; replace control/invalid bytes for a valid JSON label. */
    int j=0;
    for(int i=0;i<count;) {
        unsigned char c=raw[i];int len=c<128?1:c>=194&&c<=223?2:c>=224&&c<=239?3:c>=240&&c<=244?4:0;
        int valid=len&&i+len<=count;
        for(int k=1;valid&&k<len;k++)if((raw[i+k]&192)!=128)valid=0;
        if(valid&&len==3&&((c==224&&raw[i+1]<160)||(c==237&&raw[i+1]>=160)))valid=0;
        if(valid&&len==4&&((c==240&&raw[i+1]<144)||(c==244&&raw[i+1]>=144)))valid=0;
        if(!valid||c<32||c==127){n->ssid[j++]='?';i++;}else{memcpy(n->ssid+j,raw+i,len);j+=len;i+=len;}
    }n->ssid[j]=0;
}
static int open_control(char *path,size_t size,const char *kind) {
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
    struct sockaddr_un local={.sun_family=AF_UNIX},remote={.sun_family=AF_UNIX};
    snprintf(path,size,"/var/run/e1-wifi-%ld-%s",(long)getpid(),kind);snprintf(local.sun_path,sizeof local.sun_path,"%s",path);
    snprintf(remote.sun_path,sizeof remote.sun_path,"%s",CONTROL);unlink(path);
    struct timeval wait={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&wait,sizeof wait);
    if(bind(fd,(void*)&local,sizeof local)||connect(fd,(void*)&remote,sizeof remote)){close(fd);unlink(path);return -1;}
    chmod(path,0600);return fd;
}
static int command_fd(int fd,const char *cmd,char *out,size_t cap) {
    if(fd<0||send(fd,cmd,strlen(cmd),0)<0)return -1;
    ssize_t n=recv(fd,out,cap-1,0);if(n<0)return -1;out[n]=0;return (int)n;
}
static int command(const char *cmd,char *out,size_t cap) {return command_fd(ctrl,cmd,out,cap);}
static int ok(const char *cmd) {char out[128];return command(cmd,out,sizeof out)>=0&&!strncmp(out,"OK",2);}
static int numbered(const char *verb,int id) {char cmd[96];snprintf(cmd,sizeof cmd,"%s %d",verb,id);return ok(cmd);}
static void close_controls(void) {
    if(ctrl>=0)close(ctrl);if(monitor>=0)close(monitor);ctrl=monitor=-1;available=0;
    if(*ctrl_path)unlink(ctrl_path);if(*monitor_path)unlink(monitor_path);
}
static int ensure_service(void) {
    char out[128];if(ctrl>=0&&command("PING",out,sizeof out)>0&&!strncmp(out,"PONG",4))return 1;
    close_controls();
    ctrl=open_control(ctrl_path,sizeof ctrl_path,"cmd");
    if(ctrl<0) {
        /* Match the factory service/config, without putting credentials in argv. */
        system("/sbin/ifconfig wlan0 up");
        system("/usr/sbin/wpa_supplicant -B -i wlan0 -c " CONFIG " -C /var/run/wpa_supplicant >/dev/null 2>&1");
        ctrl=open_control(ctrl_path,sizeof ctrl_path,"cmd");
    }
    if(ctrl<0){snprintf(message,sizeof message,"Cannot start Wi-Fi service");return 0;}
    monitor=open_control(monitor_path,sizeof monitor_path,"events");
    if(monitor<0||command_fd(monitor,"ATTACH",out,sizeof out)<0||strncmp(out,"OK",2)){close_controls();snprintf(message,sizeof message,"Cannot monitor Wi-Fi service");return 0;}
    available=1;
    /* A lost command reply must not leave an unsaved candidate selected. */
    ok("RECONFIGURE");
    if(connecting){connecting=0;candidate=target=replaced=-1;snprintf(message,sizeof message,"Wi-Fi service restarted; try connecting again");}
    return 1;
}
static void field(const char *text,const char *key,char *out,size_t cap) {
    out[0]=0;size_t len=strlen(key);
    for(const char *p=text;*p;) {if(!strncmp(p,key,len)&&p[len]=='='){p+=len+1;size_t n=strcspn(p,"\n\r");snprintf(out,cap,"%.*s",(int)n,p);return;}p=strchr(p,'\n');if(!p)return;p++;}
}
static void ip_address(void) {
    address[0]=0;int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return;
    struct ifreq req={0};strcpy(req.ifr_name,"wlan0");
    if(!ioctl(fd,SIOCGIFADDR,&req))inet_ntop(AF_INET,&((struct sockaddr_in*)&req.ifr_addr)->sin_addr,address,sizeof address);close(fd);
}
static void read_networks(void) {
    Network saved[64];int count=0;char out[32768];net_count=0;
    if(command("LIST_NETWORKS",out,sizeof out)<0)return;
    char *save=NULL,*line=strtok_r(out,"\n",&save);
    while((line=strtok_r(NULL,"\n",&save))&&count<64) {
        char *tab=strchr(line,'\t');if(!tab)continue;*tab++=0;char *end=strchr(tab,'\t');if(!end)continue;*end=0;
        Network *n=&saved[count++];memset(n,0,sizeof *n);n->id=atoi(line);n->signal=-100;n->supported=1;decode_ssid(tab,n);strcpy(n->security,"saved");
        char cmd[64],security[128];snprintf(cmd,sizeof cmd,"GET_NETWORK %d key_mgmt",n->id);
        if(command(cmd,security,sizeof security)>0){if(strstr(security,"WPA-PSK"))strcpy(n->security,"WPA-PSK");else if(!strncmp(security,"NONE",4))strcpy(n->security,"open");else{strcpy(n->security,"unsupported");n->supported=0;}}
    }
    if(command("SCAN_RESULTS",out,sizeof out)>0) {
        save=NULL;line=strtok_r(out,"\n",&save);
        while((line=strtok_r(NULL,"\n",&save))&&net_count<MAX_NETS-64) {
            char *cols[5]={line};int valid=1;
            for(int i=1;i<5;i++){char *t=strchr(cols[i-1],'\t');if(!t){valid=0;break;}*t=0;cols[i]=t+1;}if(!valid||!*cols[4])continue;
            Network n={.id=-1,.signal=atoi(cols[2]),.supported=1};decode_ssid(cols[4],&n);
            if(strstr(cols[3],"PSK"))strcpy(n.security,"WPA-PSK");
            else if(strstr(cols[3],"WPA")||strstr(cols[3],"RSN")||strstr(cols[3],"WEP")||strstr(cols[3],"SAE")||strstr(cols[3],"OWE")){strcpy(n.security,"unsupported");n.supported=0;}
            else strcpy(n.security,"open");
            int duplicate=-1;for(int i=0;i<net_count;i++)if(!strcmp(nets[i].hex,n.hex)&&!strcmp(nets[i].security,n.security)){duplicate=i;break;}
            if(duplicate>=0){if(n.signal>nets[duplicate].signal)nets[duplicate].signal=n.signal;continue;}
            for(int i=0;i<count;i++)if(!strcmp(saved[i].hex,n.hex)&&!strcmp(saved[i].security,n.security)){n.id=saved[i].id;break;}nets[net_count++]=n;
        }
    }
    for(int i=0;i<count&&net_count<MAX_NETS;i++) {
        int found=0;for(int j=0;j<net_count;j++)if(nets[j].id==saved[i].id){found=1;break;}if(!found)nets[net_count++]=saved[i];
    }
    for(int i=0;i<net_count;i++)for(int j=i+1;j<net_count;j++)if(nets[j].signal>nets[i].signal){Network n=nets[i];nets[i]=nets[j];nets[j]=n;}
}
static void publish(void) {
    char *json=NULL;size_t len=0;FILE *f=open_memstream(&json,&len);if(!f)return;
    fprintf(f,"{\"enabled\":%s,\"available\":%s,\"scanning\":%s,\"connecting\":%s,\"status\":",enabled?"true":"false",available?"true":"false",scanning?"true":"false",connecting?"true":"false");json_string(f,state);
    fputs(",\"ssid\":",f);json_string(f,current_ssid);fputs(",\"ip\":",f);json_string(f,address);fputs(",\"message\":",f);json_string(f,message);fputs(",\"networks\":[",f);
    for(int i=0;i<net_count;i++){Network *n=&nets[i];if(i)fputc(',',f);fputs("{\"ssid\":",f);json_string(f,n->ssid);fputs(",\"ssidHex\":",f);json_string(f,n->hex);fputs(",\"security\":",f);json_string(f,n->security);fprintf(f,",\"id\":%d,\"saved\":%s,\"signal\":%d,\"supported\":%s}",n->id,n->id>=0?"true":"false",n->signal,n->supported?"true":"false");}
    fputs("]}",f);fclose(f);pthread_mutex_lock(&W.mutex);free(W.json);W.json=json;pthread_mutex_unlock(&W.mutex);
}
static void scan(void) {
    if(ok("SCAN")){scanning=1;scan_until=now_ms()+12000;}
}
static void rollback(const char *reason) {
    if(candidate>=0)numbered("REMOVE_NETWORK",candidate);
    /* Restore saved enabled/disabled flags; tentative profiles were never saved. */
    ok("RECONFIGURE");ok("RECONNECT");connecting=0;candidate=target=replaced=-1;snprintf(message,sizeof message,"%s",reason);read_networks();
}
static void connect_job(Job *job) {
    if(connecting)rollback("");
    read_networks();int id=-1;
    for(int i=0;i<net_count;i++)if(!strcmp(nets[i].hex,job->hex)&&nets[i].id>=0&&(!strcmp(job->security,"saved")||!strcmp(nets[i].security,job->security))){id=nets[i].id;break;}
    if(!strcmp(job->security,"saved")) {
        if(id<0){snprintf(message,sizeof message,"Saved network no longer exists; scan again");return;}
    } else {
        unsigned char raw[32],key[32];int ssid_len=decode_hex(job->hex,raw);char psk[65]={0};
        if(!strcmp(job->security,"WPA-PSK")) {
            if(strlen(job->password)==64)strcpy(psk,job->password);
            else {
                void *lib=dlopen("libcrypto.so.1.0.0",RTLD_NOW|RTLD_LOCAL);
                typedef int (*Pbkdf)(const char *,int,const unsigned char *,int,int,int,unsigned char *);
                Pbkdf derive=lib?(Pbkdf)dlsym(lib,"PKCS5_PBKDF2_HMAC_SHA1"):NULL;
                int result=derive?derive(job->password,strlen(job->password),raw,ssid_len,4096,32,key):0;
                if(lib)dlclose(lib);
                if(!result){snprintf(message,sizeof message,"Password key derivation unavailable");wipe(key,sizeof key);return;}
                for(int i=0;i<32;i++)sprintf(psk+2*i,"%02x",key[i]);wipe(key,sizeof key);
            }
        }
        char reply[128],cmd[180];
        if(command("ADD_NETWORK",reply,sizeof reply)<0||!isdigit((unsigned char)reply[0])){snprintf(message,sizeof message,"Cannot create network profile");wipe(psk,sizeof psk);return;}
        replaced=id;id=atoi(reply);candidate=id;
        snprintf(cmd,sizeof cmd,"SET_NETWORK %d ssid %s",id,job->hex);int good=ok(cmd);
        snprintf(cmd,sizeof cmd,"SET_NETWORK %d key_mgmt %s",id,!strcmp(job->security,"open")?"NONE":"WPA-PSK");good=ok(cmd)&&good;
        if(psk[0]){snprintf(cmd,sizeof cmd,"SET_NETWORK %d psk %s",id,psk);good=ok(cmd)&&good;}wipe(psk,sizeof psk);wipe(cmd,sizeof cmd);
        if(!good){rollback("Cannot configure this network");return;}
    }
    if(!numbered("SELECT_NETWORK",id)){rollback("Cannot select this network");return;}
    target=id;connecting=1;connect_until=now_ms()+35000;message[0]=0;strcpy(state,"Connecting");
}
static void process_job(Job *j) {
    message[0]=0;
    if(j->job==JOB_ENABLE) {
        enabled=j->enabled;
        if(!enabled) {
            if(connecting)rollback("");ok("DISCONNECT");system("/sbin/ifconfig wlan0 down");
            int fd=open(OFF_FILE,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0){fsync(fd);close(fd);}else snprintf(message,sizeof message,"Cannot save Wi-Fi preference");
            scanning=0;current_ssid[0]=address[0]=0;strcpy(state,"Off");
        }else {unlink(OFF_FILE);system("/sbin/ifconfig wlan0 up");if(ensure_service()){ok("RECONNECT");scan();}}
        return;
    }
    if(!enabled||!ensure_service()){snprintf(message,sizeof message,"Enable Wi-Fi first");return;}
    if(j->job==JOB_SCAN)scan();
    else if(j->job==JOB_CONNECT)connect_job(j);
    else if(j->job==JOB_CANCEL){if(connecting)rollback("Connection cancelled");}
    else if(j->job==JOB_FORGET) {
        if(connecting)rollback("");
        if(!numbered("REMOVE_NETWORK",j->id)||!ok("SAVE_CONFIG"))snprintf(message,sizeof message,"Could not forget network");
        chmod(CONFIG,0600);read_networks();
    }
}
static void update_status(void) {
    char reply[4096],wpa_state[64],id_text[32],encoded[129];
    if(command("STATUS",reply,sizeof reply)<0){close_controls();strcpy(state,"Unavailable");return;}
    field(reply,"wpa_state",wpa_state,sizeof wpa_state);field(reply,"ssid",encoded,sizeof encoded);field(reply,"id",id_text,sizeof id_text);
    Network label={0};decode_ssid(encoded,&label);snprintf(current_ssid,sizeof current_ssid,"%s",label.ssid);
    int complete=!strcmp(wpa_state,"COMPLETED");
    if(complete)ip_address();else address[0]=0;
    snprintf(state,sizeof state,"%s",complete?(*address?"Connected":"Getting IP address"):connecting?"Connecting":"Disconnected");
    if(connecting&&complete&&*id_text&&atoi(id_text)==target) {
        /* Persist only authenticated profiles, keeping other saved networks usable. */
        if(candidate>=0&&replaced>=0)numbered("REMOVE_NETWORK",replaced);
        ok("ENABLE_NETWORK all");
        if(!ok("SAVE_CONFIG"))snprintf(message,sizeof message,"Connected, but could not save network");
        chmod(CONFIG,0600);connecting=0;candidate=target=replaced=-1;read_networks();
        system("/sbin/dhcpcd -n wlan0 >/dev/null 2>&1");
    }else if(connecting&&now_ms()>connect_until)rollback("Connection timed out. Check password and signal.");
}
static void events(void) {
    char event[4096];
    while(monitor>=0) {
        ssize_t n=recv(monitor,event,sizeof event-1,MSG_DONTWAIT);if(n<=0)break;event[n]=0;
        if(strstr(event,"CTRL-EVENT-SCAN-RESULTS")){scanning=0;read_networks();}
        char *id=strstr(event," id=");
        if(connecting&&id&&atoi(id+4)==target&&strstr(event,"SSID-TEMP-DISABLED")&&strstr(event,"WRONG_KEY")){rollback("Incorrect password. Try again.");break;}
    }
}
static void backup_config(void) {
    int in=open(CONFIG,O_RDONLY);if(in<0)return;
    int out=open("/userdata/e1-recorder/wpa_supplicant.factory.conf",O_WRONLY|O_CREAT|O_EXCL,0600);
    if(out>=0){char bytes[4096];ssize_t n;while((n=read(in,bytes,sizeof bytes))>0){ssize_t at=0;while(at<n){ssize_t w=write(out,bytes+at,n-at);if(w<=0)break;at+=w;}if(at<n)break;}fsync(out);close(out);wipe(bytes,sizeof bytes);}close(in);chmod(CONFIG,0600);
}
static void *worker(void *unused) {
    (void)unused;backup_config();enabled=access(OFF_FILE,F_OK)!=0;
    if(enabled&&ensure_service()){ok("RECONFIGURE");ok("RECONNECT");read_networks();scan();}
    else if(!enabled){system("/sbin/ifconfig wlan0 down");strcpy(state,"Off");}
    unsigned int ticks=0;
    for(;;) {
        Job job={0};pthread_mutex_lock(&W.mutex);int running=W.running;
        if(running){job=W.next;wipe(&W.next,sizeof W.next);}pthread_mutex_unlock(&W.mutex);if(!running)break;
        if(job.job)process_job(&job);wipe(&job,sizeof job);
        if(enabled){events();if(ticks%5==0){if(ensure_service())update_status();}if(scanning&&now_ms()>scan_until){scanning=0;read_networks();}}
        publish();ticks++;usleep(200000);
    }
    if(connecting)rollback("");close_controls();return NULL;
}
int wifi_start(void) {
    W.running=1;W.started=pthread_create(&W.thread,NULL,worker,NULL)==0;if(!W.started)W.running=0;return W.started?0:-1;
}
void wifi_stop(void) {
    pthread_mutex_lock(&W.mutex);W.running=0;wipe(&W.next,sizeof W.next);pthread_mutex_unlock(&W.mutex);
    if(W.started)pthread_join(W.thread,NULL);W.started=0;
}
char *wifi_json(void) {
    pthread_mutex_lock(&W.mutex);char *json=strdup(W.json?W.json:"{\"status\":\"Starting\",\"enabled\":true,\"available\":false,\"connecting\":false,\"scanning\":false,\"ssid\":\"\",\"ip\":\"\",\"message\":\"\",\"networks\":[]}");pthread_mutex_unlock(&W.mutex);return json;
}
/* Strict form decoding: reject truncation, invalid escapes and embedded NULs. */
static int form_value(const char *form,const char *key,char *out,size_t cap) {
    size_t k=strlen(key);out[0]=0;
    for(const char *p=form;*p;) {
        const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        if((size_t)(end-p)>k&&p[k]=='='&&!strncmp(p,key,k)) {
            size_t n=0;p+=k+1;
            while(p<end){unsigned char c=*p++;if(c=='%'){if(end-p<2||unhex(p[0])<0||unhex(p[1])<0)return -1;c=unhex(p[0])*16+unhex(p[1]);p+=2;}else if(c=='+')c=' ';if(!c||n+1>=cap)return -1;out[n++]=c;}out[n]=0;return 1;
        }p=*end?end+1:end;
    }return 0;
}
int wifi_submit(const char *action,const char *form,const char **error) {
    Job j={0};const char *err="Invalid Wi-Fi request";
    if(!strcmp(action,"scan"))j.job=JOB_SCAN;
    else if(!strcmp(action,"cancel"))j.job=JOB_CANCEL;
    else if(!strcmp(action,"enable")){char value[8];if(form_value(form,"enabled",value,sizeof value)!=1||(strcmp(value,"1")&&strcmp(value,"0")))goto bad;j.job=JOB_ENABLE;j.enabled=!strcmp(value,"1");}
    else if(!strcmp(action,"forget")){char value[16];if(form_value(form,"id",value,sizeof value)!=1||!*value)goto bad;for(char *p=value;*p;p++)if(!isdigit((unsigned char)*p))goto bad;j.id=atoi(value);if(j.id<0||j.id>100000)goto bad;j.job=JOB_FORGET;}
    else if(!strcmp(action,"connect")) {
        unsigned char raw[32];if(form_value(form,"ssid",j.hex,sizeof j.hex)!=1||decode_hex(j.hex,raw)<1)goto bad;
        if(form_value(form,"security",j.security,sizeof j.security)!=1||form_value(form,"password",j.password,sizeof j.password)<0)goto bad;
        if(strcmp(j.security,"saved")&&strcmp(j.security,"open")&&strcmp(j.security,"WPA-PSK")){err="Only open and WPA/WPA2 personal networks are supported";goto bad;}
        if(!strcmp(j.security,"WPA-PSK")){size_t n=strlen(j.password);err="Use 8–63 password bytes or a 64-digit hexadecimal key";if(n<8||n>64)goto bad;for(size_t i=0;i<n;i++)if((n==64&&unhex(j.password[i])<0)||(n<64&&((unsigned char)j.password[i]<32||(unsigned char)j.password[i]>126)))goto bad;}
        j.job=JOB_CONNECT;
    }else goto bad;
    pthread_mutex_lock(&W.mutex);
    if(!W.running||W.next.job){pthread_mutex_unlock(&W.mutex);wipe(&j,sizeof j);*error="Wi-Fi service is busy; try again";return 409;}
    W.next=j;pthread_mutex_unlock(&W.mutex);wipe(&j,sizeof j);return 200;
 bad: wipe(&j,sizeof j);*error=err;return 400;
}
