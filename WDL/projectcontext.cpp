#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <string.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "projectcontext.h"

#include "fileread.h"
#include "filewrite.h"
#include "heapbuf.h"
#include "wdlstring.h"
#include "lineparse.h"



class ProjectStateContext_Mem : public ProjectStateContext
{
public:
  ProjectStateContext_Mem(WDL_HeapBuf *hb) { m_heapbuf=hb; m_pos=0; }
  virtual ~ProjectStateContext_Mem() 
  { 
  };


  virtual void AddLine(const char *fmt, ...);
  virtual int GetLine(char *buf, int buflen); // returns -1 on eof

  virtual WDL_INT64 GetOutputSize() { return m_heapbuf ? m_heapbuf->GetSize() : 0; }

  int m_pos;
  WDL_HeapBuf *m_heapbuf;
};

void ProjectStateContext_Mem::AddLine(const char *fmt, ...)
{
  if (!m_heapbuf) return;

  char buf[4096];
  va_list va;
  va_start(va,fmt);
  // todo: a more compact representation for things such as %f?
  int l=vsprintf(buf,fmt,va);
  va_end(va);

  if (l<=0) return;

  l++;

  int sz=m_heapbuf->GetSize();
  if (!sz)
  {
    m_heapbuf->SetGranul(256*1024);
  }

  int newsz = sz + l;
  char *p = (char *)m_heapbuf->Resize(newsz);
  if (m_heapbuf->GetSize() != newsz)
  {
    // ERROR, resize to 0 and return
    m_heapbuf->Resize(0);
    m_heapbuf=0;
    return; 
  }
  memcpy(p+sz,buf,l);
}

int ProjectStateContext_Mem::GetLine(char *buf, int buflen) // returns -1 on eof
{
  if (!m_heapbuf) return -1;

  buf[0]=0;
  if (m_pos >= m_heapbuf->GetSize()) return -1;
  char *curptr=(char *)m_heapbuf->Get() + m_pos;
  
  int cpl = strlen(curptr);
  if (buflen > 0)
  {
    int a = buflen-1;
    if (a > cpl) a=cpl;
    memcpy(buf,curptr,a);
    buf[a]=0;
  }
  m_pos += cpl+1;
  return 0;
}

class ProjectStateContext_File : public ProjectStateContext
{
public:
  ProjectStateContext_File(WDL_FileRead *rd, WDL_FileWrite *wr) 
  { 
    m_rd=rd; 
    m_wr=wr; 
    m_indent=0; 
    m_bytesOut=0;
    m_errcnt=false; 
  }
  virtual ~ProjectStateContext_File(){ delete m_rd; delete m_wr; };

  virtual void AddLine(const char *fmt, ...);
  virtual int GetLine(char *buf, int buflen); // returns -1 on eof

  virtual WDL_INT64 GetOutputSize() { return m_bytesOut; }

  bool HasError() { return m_errcnt; }
  WDL_FileRead *m_rd;
  WDL_FileWrite *m_wr;
  bool m_errcnt;
  int m_indent;

  WDL_INT64 m_bytesOut;
};

int ProjectStateContext_File::GetLine(char *buf, int buflen)
{
  if (!m_rd||buflen<2) return -1;

  buf[0]=0;
  for (;;)
  {
    buf[0]=0;
    int i=0;
    while (i<buflen-1)
    {
      if (!m_rd->Read(buf+i,1)) { if (!i) return -1; break; }

      if (buf[i] == '\r' || buf[i] == '\n')  
      {
        if (!i) continue; // skip over leading newlines
        break;
      }

      if (!i && (buf[0] == ' ' || buf[0] == '\t')) continue; // skip leading blanks

      i++;
    }
    if (i<1) continue;

    buf[i]=0;

    if (buf[0]) return 0;
  }
  return -1;
}

void ProjectStateContext_File::AddLine(const char *fmt, ...)
{
  if (m_wr && !m_errcnt)
  {
    int err=0;

    int a=m_indent;
    if (fmt[0] == '<') m_indent+=2;
    else if (fmt[0] == '>') a=(m_indent-=2);
    
    if (a>0) 
    {
      m_bytesOut+=a;
      char tb[32];
      memset(tb,' ',sizeof(tb));
      while (a>0) 
      {
        int tl = a;
        if (tl>32)tl=32;
        a-=tl;     
        m_wr->Write(tb,tl);
      }
    }


    char buf[8192];
    va_list va;
    va_start(va,fmt);
#ifdef _WIN32
    int buflen = _vsnprintf(buf,sizeof(buf),fmt,va);
#else
    int buflen = vsnprintf(buf,sizeof(buf),fmt,va);
#endif
    va_end(va);
    err |= m_wr->Write(buf,buflen) != buflen;
    err |= m_wr->Write("\r\n",2) != 2;
    m_bytesOut += 2 + buflen;

    if (err) m_errcnt=true;
  }
}



ProjectStateContext *ProjectCreateFileRead(const char *fn)
{
  WDL_FileRead *rd = new WDL_FileRead(fn);
  if (!rd || !rd->IsOpen())
  {
    delete rd;
    return NULL;
  }
  return new ProjectStateContext_File(rd,NULL);
}
ProjectStateContext *ProjectCreateFileWrite(const char *fn)
{
  WDL_FileWrite *wr = new WDL_FileWrite(fn);
  if (!wr || !wr->IsOpen())
  {
    delete wr;
    return NULL;
  }
  return new ProjectStateContext_File(NULL,wr);
}


ProjectStateContext *ProjectCreateMemCtx(WDL_HeapBuf *hb)
{
  return new ProjectStateContext_Mem(hb);
}

bool ProjectContext_GetNextLine(ProjectStateContext *ctx, LineParser *lpOut)
{
  for (;;)
  {
    char linebuf[4096];
    if (ctx->GetLine(linebuf,sizeof(linebuf))) 
    {
      lpOut->parse("");
      return false;
    }

    if (lpOut->parse(linebuf)||lpOut->getnumtokens()<=0) continue;
    
    return true; // success!

  }
}


bool ProjectContext_EatCurrentBlock(ProjectStateContext *ctx)
{
  int child_count=1;
  if (ctx) for (;;)
  {
    char linebuf[4096];
    if (ctx->GetLine(linebuf,sizeof(linebuf))) break;

    bool comment_state=false;
    LineParser lp(comment_state);
    if (lp.parse(linebuf)||lp.getnumtokens()<=0) continue;
    if (lp.gettoken_str(0)[0] == '>')  if (--child_count < 1) return true;
    if (lp.gettoken_str(0)[0] == '<') child_count++;
  }

  return false;
}


static void pc_base64encode(const unsigned char *in, char *out, int len)
{
  char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int shift = 0;
  int accum = 0;

  while (len>0)
  {
    len--;
    accum <<= 8;
    shift += 8;
    accum |= *in++;
    while ( shift >= 6 )
    {
      shift -= 6;
      *out++ = alphabet[(accum >> shift) & 0x3F];
    }
  }
  if (shift == 4)
  {
    *out++ = alphabet[(accum & 0xF)<<2];
    *out++='=';  
  }
  else if (shift == 2)
  {
    *out++ = alphabet[(accum & 0x3)<<4];
    *out++='=';  
    *out++='=';  
  }

  *out++=0;
}

static int pc_base64decode(const char *src, unsigned char *dest, int destsize)
{
  unsigned char *olddest=dest;

  int accum=0;
  int nbits=0;
  while (*src)
  {
    int x=0;
    char c=*src++;
    if (c >= 'A' && c <= 'Z') x=c-'A';
    else if (c >= 'a' && c <= 'z') x=c-'a' + 26;
    else if (c >= '0' && c <= '9') x=c-'0' + 52;
    else if (c == '+') x=62;
    else if (c == '/') x=63;
    else break;

    accum <<= 6;
    accum |= x;
    nbits += 6;   

    while (nbits >= 8)
    {
      if (--destsize<0) break;
      nbits-=8;
      *dest++ = (char)((accum>>nbits)&0xff);
    }

  }
  return dest-olddest;
}


int cfg_decode_binary(ProjectStateContext *ctx, WDL_HeapBuf *hb) // 0 on success, doesnt clear hb
{
  int child_count=1;
  bool comment_state=false;
  for (;;)
  {
    char linebuf[4096];
    if (ctx->GetLine(linebuf,sizeof(linebuf))) break;

    LineParser lp(comment_state);
    if (lp.parse(linebuf)||lp.getnumtokens()<=0) continue;

    if (lp.gettoken_str(0)[0] == '<') child_count++;
    else if (lp.gettoken_str(0)[0] == '>') { if (child_count-- == 1) return 0; }
    else if (child_count == 1)
    {     
      unsigned char buf[8192];
      int buf_l=pc_base64decode(lp.gettoken_str(0),buf,sizeof(buf));
      int os=hb->GetSize();
      hb->Resize(os+buf_l);
      memcpy((char *)hb->Get()+os,buf,buf_l);
    }
  }
  return -1;  
}

void cfg_encode_binary(ProjectStateContext *ctx, const void *ptr, int len)
{
  const unsigned char *p=(const unsigned char *)ptr;
  while (len>0)
  {
    char buf[80];
    int thiss=len;
    if (thiss > 40) thiss=40;
    pc_base64encode(p,buf,thiss);

    ctx->AddLine(buf);
    p+=thiss;
    len-=thiss;
  }
}


int cfg_decode_textblock(ProjectStateContext *ctx, WDL_String *str) // 0 on success, appends to str
{
  int child_count=1;
  bool comment_state=false;
  for (;;)
  {
    char linebuf[4096];
    if (ctx->GetLine(linebuf,sizeof(linebuf))) break;

    if (!linebuf[0]) continue;
    LineParser lp(comment_state);
    if (!lp.parse(linebuf)&&lp.getnumtokens()>0) 
    {
      if (lp.gettoken_str(0)[0] == '<') { child_count++; continue; }
      else if (lp.gettoken_str(0)[0] == '>') { if (child_count-- == 1) return 0; continue; }
    }
    if (child_count == 1)
    {     
      char *p=linebuf;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '|')
      {
        if (str->Get()[0]) str->Append("\r\n");
        str->Append(++p);
      }
    }
  }
  return -1;  
}

void cfg_encode_textblock(ProjectStateContext *ctx, const char *text)
{
  WDL_String tmpcopy(text);
  char *txt=(char*)tmpcopy.Get();
  while (*txt)
  {
    char *ntext=txt;
    while (*ntext && *ntext != '\r' && *ntext != '\n') ntext++;
    if (ntext > txt || *ntext)
    {
      char ov=*ntext;
      *ntext=0;
      ctx->AddLine("|%s",txt);
      *ntext=ov;
    }
    txt=ntext;
    if (*txt == '\r')
    {
      if (*++txt== '\n') txt++;
    }
    else if (*txt == '\n')
    {
      if (*++txt == '\r') txt++;
    }
  }
}


void makeEscapedConfigString(const char *in, WDL_String *out)
{
  int flags=0;
  const char *p=in;
  while (*p && flags!=7)
  {
    char c=*p++;
    if (c=='"') flags|=1;
    else if (c=='\'') flags|=2;
    else if (c=='`') flags|=4;
  }
  if (flags!=7)
  {
    const char *src=(flags&1)?((flags&2)?"`":"'"):"\"";
    out->Set(src);
    out->Append(in);
    out->Append(src);
  }
  else  // ick, change ` into '
  {
    out->Set("`");
    out->Append(in);
    out->Append("`");
    char *p=out->Get()+1;
    while (*p && p[1])
    {
      if (*p == '`') *p='\'';
      p++;
    }
  }
}
