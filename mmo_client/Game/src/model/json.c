#include "model/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* skip_ws(const char* p,const char* end){while(p<end&&isspace((unsigned char)*p))p++;return p;}
static const char* skip_string(const char*p,const char*end){
    if(p>=end||*p!='"')return NULL;
    p++;
    while(p<end){if(*p=='\\'){p+=2;continue;}if(*p++=='"')return p;}return NULL;
}
static const char* skip_value(const char*p,const char*end){
    p=skip_ws(p,end);if(p>=end)return NULL;if(*p=='"')return skip_string(p,end);
    if(*p=='{'||*p=='['){char open=*p,close=open=='{'?'}':']';int depth=0;int quoted=0,escaped=0;
        for(;p<end;p++){char c=*p;if(quoted){if(escaped)escaped=0;else if(c=='\\')escaped=1;else if(c=='"')quoted=0;continue;}
            if(c=='"'){quoted=1;continue;}if(c==open)depth++;else if(c==close&&--depth==0)return p+1;}return NULL;}
    while(p<end&&!isspace((unsigned char)*p)&&*p!=','&&*p!=']'&&*p!='}')p++;
    return p;
}
JsonSlice json_slice(const char*text){JsonSlice s={text,text?strlen(text):0};return s;}
char*json_read_file(const char*path,char*error,int error_size){FILE*f=fopen(path,"rb");if(!f){if(error&&error_size)snprintf(error,(size_t)error_size,"could not open %s",path);return NULL;}if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long n=ftell(f);if(n<0){fclose(f);return NULL;}rewind(f);char*s=malloc((size_t)n+1);if(!s){fclose(f);return NULL;}if(fread(s,1,(size_t)n,f)!=(size_t)n){free(s);fclose(f);return NULL;}s[n]=0;fclose(f);return s;}
int json_member(JsonSlice object,const char*key,JsonSlice*out){
    if(!object.begin||!key||!out)return 0;
    const char*p=object.begin,*end=p+object.length;p=skip_ws(p,end);if(p>=end||*p!='{')return 0;p++;
    while((p=skip_ws(p,end))<end&&*p!='}'){
        const char*q=skip_string(p,end);if(!q)return 0;size_t length=(size_t)(q-p-2);int match=strlen(key)==length&&memcmp(p+1,key,length)==0;
        p=skip_ws(q,end);if(p>=end||*p++!=':')return 0;const char*v=skip_ws(p,end);const char*next=skip_value(v,end);if(!next)return 0;
        if(match){out->begin=v;out->length=(size_t)(next-v);return 1;}p=skip_ws(next,end);if(p<end&&*p==',')p++;
    }return 0;
}
int json_string(JsonSlice v,char*out,size_t cap){if(!out||!cap||v.length<2||v.begin[0]!='"')return 0;size_t n=0;const char*p=v.begin+1,*end=v.begin+v.length;while(p<end&&*p!='"'){char c=*p++;if(c=='\\'){if(p>=end)return 0;c=*p++;if(c=='n')c='\n';else if(c=='t')c='\t';else if(c!='"'&&c!='\\'&&c!='/')return 0;}if(n+1>=cap)return 0;out[n++]=c;}if(p>=end||*p!='"')return 0;out[n]=0;return 1;}
int json_int(JsonSlice v,int*out){if(!out||!v.begin)return 0;char b[32];if(v.length>=sizeof(b))return 0;memcpy(b,v.begin,v.length);b[v.length]=0;char*e;long n=strtol(b,&e,10);if(e==b)return 0;*out=(int)n;return 1;}
int json_float(JsonSlice v,float*out){if(!out||!v.begin)return 0;char b[48];if(v.length>=sizeof(b))return 0;memcpy(b,v.begin,v.length);b[v.length]=0;char*e;float n=strtof(b,&e);if(e==b)return 0;*out=n;return 1;}
int json_array_count(JsonSlice a){if(!a.begin)return-1;const char*p=skip_ws(a.begin,a.begin+a.length),*end=a.begin+a.length;if(p>=end||*p++!='[')return-1;int n=0;for(;;){p=skip_ws(p,end);if(p>=end)return-1;if(*p==']')return n;const char*q=skip_value(p,end);if(!q)return-1;n++;p=skip_ws(q,end);if(p<end&&*p==',')p++;else if(p>=end||*p!=']')return-1;}}
int json_array_at(JsonSlice a,int index,JsonSlice*out){if(index<0||!out)return 0;const char*p=skip_ws(a.begin,a.begin+a.length),*end=a.begin+a.length;if(p>=end||*p++!='[')return 0;for(int i=0;;i++){p=skip_ws(p,end);if(p>=end||*p==']')return 0;const char*q=skip_value(p,end);if(!q)return 0;if(i==index){out->begin=p;out->length=(size_t)(q-p);return 1;}p=skip_ws(q,end);if(p<end&&*p==',')p++;}}
int json_array_index(JsonSlice a,JsonSlice*out,int capacity){if(!a.begin||!out||capacity<0)return-1;const char*p=skip_ws(a.begin,a.begin+a.length),*end=a.begin+a.length;if(p>=end||*p++!='[')return-1;int n=0;for(;;){p=skip_ws(p,end);if(p>=end)return-1;if(*p==']')return n;const char*q=skip_value(p,end);if(!q)return-1;if(n>=capacity)return-1;out[n].begin=p;out[n].length=(size_t)(q-p);n++;p=skip_ws(q,end);if(p<end&&*p==',')p++;else if(p>=end||*p!=']')return-1;}}
