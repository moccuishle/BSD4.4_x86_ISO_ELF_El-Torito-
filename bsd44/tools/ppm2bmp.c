#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
 FILE*in=fopen(argv[1],"rb"),*out; int w,h,maxv,i,j,row,pad,fs,is; unsigned char*p; unsigned char hdr[54];
 if(!in){perror("in");return 1;}
 if(fscanf(in,"P6 %d %d %d",&w,&h,&maxv)!=3){fprintf(stderr,"bad ppm\n");return 2;}
 fgetc(in);
 p=malloc((size_t)w*h*3); if(!p)return 3;
 if(fread(p,1,(size_t)w*h*3,in)!=(size_t)w*h*3){fprintf(stderr,"short\n");return 4;}
 fclose(in);
 out=fopen(argv[2],"wb"); if(!out)return 5;
 row=(w*3+3)&~3; pad=row-w*3; fs=54+row*h; is=row*h;
 memset(hdr,0,54); hdr[0]='B';hdr[1]='M'; memcpy(hdr+2,&fs,4); hdr[10]=54; hdr[14]=40;
 memcpy(hdr+18,&w,4); memcpy(hdr+22,&h,4); hdr[26]=1;hdr[28]=24; memcpy(hdr+34,&is,4);
 fwrite(hdr,1,54,out);
 for(j=h-1;j>=0;j--){ const unsigned char*s=p+(size_t)j*w*3;
  for(i=0;i<w;i++){ fputc(s[3*i+2],out); fputc(s[3*i+1],out); fputc(s[3*i],out);} 
  for(i=0;i<pad;i++) fputc(0,out); }
 fclose(out); free(p); return 0; }
