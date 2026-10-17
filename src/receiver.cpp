#include "receiver.h"
#include "utils.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>

static Receiver& self(void *p) { return *static_cast<Receiver *>(p); }
static void log_message(void *,int level,const char *text) {
    if (level<=LOGGER_INFO) fprintf(stderr,"[airplay] %s\n",text);
}
void Receiver::show(const std::string& s) { std::lock_guard<std::mutex> l(mutex);message=s; }
std::string Receiver::status() { std::lock_guard<std::mutex> l(mutex);return message; }
std::string Receiver::pin() { std::lock_guard<std::mutex> l(mutex);return pin_text; }
void Receiver::enqueue(bool video,unsigned codec,uint64_t time,const uint8_t *data,int size) {
    if (!data || size<=0 || size>1048576 || !time) return;
    activity=now_ns();
    if(!video) {
        last_audio_arrival=now_ns();
        int64_t offset=int64_t(audio_delay_ms.load())*1000000;
        if(offset<0 && time<uint64_t(-offset))return;
        time=offset>=0?time+uint64_t(offset):time-uint64_t(-offset);
    }
    std::lock_guard<std::mutex> l(mutex);
    if (bytes+size>4*1024*1024 || packets.size()>=512) { ++overflow; reset=true; return; }
    Packet p; p.video=video;p.codec=codec;p.timestamp=time;p.data.assign(data,data+size);
    bytes+=size; packets.push_back(std::move(p));
}
bool Receiver::pop(Packet& p,uint64_t latest) {
    std::lock_guard<std::mutex> l(mutex);
    if(packets.empty()) return false;
    // A/V callbacks arrive on different threads. Select the oldest presentation time.
    auto i=std::min_element(packets.begin(),packets.end(),[](const Packet& a,const Packet& b){return a.timestamp<b.timestamp;});
    if(i->timestamp>latest)return false;
    p=std::move(*i);bytes-=p.data.size();packets.erase(i);return true;
}
void Receiver::clear() { std::lock_guard<std::mutex> l(mutex);packets.clear();bytes=0;pin_text.clear();client.clear(); }
static bool valid_key(const char *p) {
    if(!p || strlen(p)!=44) return false;
    return strspn(p,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")==44;
}
bool Receiver::start(const std::string& dir) {
    directory=dir; mkdir(directory.c_str(),0700);
    std::ifstream settings(directory+"/audio-delay-ms");int delay;
    if(settings>>delay)audio_delay_ms=std::max(-1000,std::min(1000,delay));
    std::ifstream f(directory+"/paired.keys");std::string line;
    while(std::getline(f,line)) if(valid_key(line.c_str())) keys.insert(line);
    uint8_t mac[6];
    FILE *m=fopen((directory+"/device.id").c_str(),"rb");
    bool existing=m && fread(mac,1,6,m)==6;if(m) fclose(m);
    if(!existing) {
        if(RAND_bytes(mac,6)!=1) return false;
        mac[0]=(mac[0]&0xfe)|2;
        m=fopen((directory+"/device.id").c_str(),"wb");
        if(!m) return false;
        bool saved=fwrite(mac,1,6,m)==6;fclose(m);if(!saved) return false;
    }
    char device_id[18];snprintf(device_id,sizeof(device_id),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    int err=0;
    dnssd=dnssd_init("UxPlay webOS",12,reinterpret_cast<char *>(mac),6,1,&err);
    if(!dnssd || err) { show("AirPlay discovery failed");return false; }
    dnssd_set_airplay_features(dnssd,0,0);dnssd_set_airplay_features(dnssd,4,0);
    dnssd_set_airplay_features(dnssd,42,0);dnssd_set_airplay_features(dnssd,27,1);
    raop_callbacks_t cb={};cb.cls=this;
    cb.audio_process=[](void *p,raop_ntp_t *,audio_decode_struct *d){self(p).enqueue(false,d->ct,d->ntp_time_remote,d->data,d->data_len);};
    cb.video_process=[](void *p,raop_ntp_t *,video_decode_struct *d){if(!d->is_h265)self(p).enqueue(true,0,d->ntp_time_remote,d->data,d->data_len);};
    cb.conn_feedback=[](void *p){self(p).activity=now_ns();};
    cb.conn_reset=[](void *p,int reason){fprintf(stderr,"[session] transport reset reason=%d\n",reason);self(p).reset=true;};
    cb.video_reset=[](void *p,reset_type_t reason){fprintf(stderr,"[session] video reset reason=%d\n",int(reason));self(p).reset=true;};
    cb.video_pause=[](void *p){self(p).paused=true;};
    cb.video_resume=[](void *p){self(p).paused=false;};
    cb.audio_flush=[](void *p){self(p).audio_flush=true;};
    // UxPlay also calls video_flush for short-lived discovery HTTP connections.
    // The RTP lifecycle uses video_reset, so that is our session teardown boundary.
    cb.video_flush=[](void *){};
    cb.audio_set_client_volume=[](void *)->double{return 0.0;};
    cb.audio_set_volume=[](void *p,float db){self(p).volume=db<=-144?0:std::min(1.0f,std::pow(10.0f,db/20.0f));};
    cb.audio_get_format=[](void *p,unsigned char *,unsigned short *,bool *,bool *,uint64_t *){self(p).has_audio=true;};
    cb.video_report_size=[](void *p,float *,float *,float *w,float *h){
        if(std::isfinite(*w)&&std::isfinite(*h)&&*w>0&&*h>0&&*w<=1920&&*h<=1920) {self(p).width=int(*w);self(p).height=int(*h);}
    };
    cb.video_set_codec=[](void *,video_codec_t c)->int{return c==VIDEO_CODEC_H264?0:-1;};
    cb.mirror_video_running=[](void *p,bool running){self(p).mirror=running;};
    cb.report_client_request=[](void *p,char *id,char *model,char *name,bool *admit){
        Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);
        *admit=id && (r.client.empty()||r.client==id);
        fprintf(stderr,"[client] model=%.80s admitted=%d\n",model?model:"unknown",int(*admit));
        if(*admit) {r.client=id;r.message=std::string("Connecting: ")+(name?std::string(name).substr(0,80):"device");r.activity=now_ns();}
    };
    cb.display_pin=[](void *p,char *pin){Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);r.pin_text=pin?std::string(pin).substr(0,8):"";r.message="Enter this code on your device";};
    cb.passwd=[](void *,int *n)->const char *{*n=0;return nullptr;};
    cb.check_register=[](void *p,const char *key)->bool{Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);return valid_key(key)&&r.keys.count(key);};
    cb.register_client=[](void *p,const char *,const char *key,const char *){
        if(!valid_key(key)) return;
        Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);
        if(r.keys.count(key) || r.keys.size()>=64) return;
        std::string path=r.directory+"/paired.keys";
        FILE *file=fopen((path+".tmp").c_str(),"w");if(!file)return;
        bool ok=true;for(const auto& k:r.keys)if(fprintf(file,"%s\n",k.c_str())<0)ok=false;
        if(fprintf(file,"%s\n",key)<0)ok=false;
        if(fflush(file)!=0||fsync(fileno(file))!=0)ok=false;
        if(fclose(file)!=0)ok=false;
        if(ok&&rename((path+".tmp").c_str(),path.c_str())==0)r.keys.insert(key);
    };
    ntp_global_init();
    raop=raop_init(&cb);if(!raop) return false;
    raop_set_log_callback(raop,log_message,this);raop_set_log_level(raop,LOGGER_INFO);
    if(raop_init2(raop,0,device_id,(directory+"/server.key").c_str()))return false;
    raop_set_plist(raop,"width",1280);raop_set_plist(raop,"height",720);
    raop_set_plist(raop,"refreshRate",30);raop_set_plist(raop,"maxFPS",30);raop_set_plist(raop,"pin",0);
    raop_set_dnssd(raop,dnssd);
    unsigned short udp[3]={0,0,0},tcp[2]={0,0},port=0;
    raop_set_udp_ports(raop,udp);raop_set_tcp_ports(raop,tcp);
    if(raop_start_httpd(raop,&port)<0)return false;
    raop_set_port(raop,port);
    if(dnssd_register_raop(dnssd,port)||dnssd_register_airplay(dnssd,port))return false;
    fprintf(stderr,"[receiver] UxPlay webOS ready port=%u, paired=%zu\n",port,keys.size());
    return true;
}
void Receiver::adjust_audio_delay(int delta) {
    audio_delay_ms=std::max(-1000,std::min(1000,audio_delay_ms.load()+delta));
    const std::string path=directory+"/audio-delay-ms";
    FILE *f=fopen((path+".tmp").c_str(),"w");
    if(f) {
        bool ok=fprintf(f,"%d\n",audio_delay_ms.load())>0;
        if(fflush(f)||fsync(fileno(f)))ok=false;
        if(fclose(f))ok=false;
        if(ok)rename((path+".tmp").c_str(),path.c_str());
    }
}
void Receiver::disconnect() {
    if(raop) raop_remove_known_connections(raop);
    clear(); has_audio=false;mirror=false;paused=false;reset=false;audio_flush=false;last_audio_arrival=0;
    show("Waiting for a connection");
}
void Receiver::stop() {
    if(dnssd) {dnssd_unregister_airplay(dnssd);dnssd_unregister_raop(dnssd);}
    if(raop) {raop_destroy(raop);raop=nullptr;}
    if(dnssd) {dnssd_destroy(dnssd);dnssd=nullptr;}
}
Receiver::~Receiver() {stop();}
