/*
 * This file is part of MPlayer.
 *
 * Original author: M. Tourne
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <libsmbclient.h>
#include <unistd.h>

#include "mpvcore/mp_msg.h"
#include "stream.h"
#include "mpvcore/m_option.h"

struct priv {
    int fd;
};

static char smb_password[15];
static char smb_username[15];

static void smb_auth_fn(const char *server, const char *share,
             char *workgroup, int wgmaxlen, char *username, int unmaxlen,
	     char *password, int pwmaxlen)
{
  char temp[128];

  strcpy(temp, "LAN");
  if (temp[strlen(temp) - 1] == 0x0a)
    temp[strlen(temp) - 1] = 0x00;

  if (temp[0]) strncpy(workgroup, temp, wgmaxlen - 1);

  strcpy(temp, smb_username);
  if (temp[strlen(temp) - 1] == 0x0a)
    temp[strlen(temp) - 1] = 0x00;

  if (temp[0]) strncpy(username, temp, unmaxlen - 1);

  strcpy(temp, smb_password);
  if (temp[strlen(temp) - 1] == 0x0a)
    temp[strlen(temp) - 1] = 0x00;

   if (temp[0]) strncpy(password, temp, pwmaxlen - 1);
}

static int control(stream_t *s, int cmd, void *arg) {
  struct priv *p = s->priv;
  switch(cmd) {
    case STREAM_CTRL_GET_SIZE: {
      off_t size = smbc_lseek(p->fd,0,SEEK_END);
      smbc_lseek(p->fd,s->pos,SEEK_SET);
      if(size != (off_t)-1) {
        *(uint64_t *)arg = size;
        return 1;
      }
    }
  }
  return STREAM_UNSUPPORTED;
}

static int seek(stream_t *s,int64_t newpos) {
  struct priv *p = s->priv;
  s->pos = newpos;
  if(smbc_lseek(p->fd,s->pos,SEEK_SET)<0) {
    return 0;
  }
  return 1;
}

static int fill_buffer(stream_t *s, char* buffer, int max_len){
  struct priv *p = s->priv;
  int r = smbc_read(p->fd,buffer,max_len);
  return (r <= 0) ? -1 : r;
}

static int write_buffer(stream_t *s, char* buffer, int len) {
  struct priv *p = s->priv;
  int r;
  int wr = 0;
  while (wr < len) {
    r = smbc_write(p->fd,buffer,len);
    if (r <= 0)
      return -1;
    wr += r;
    buffer += r;
  }
  return len;
}

static void close_f(stream_t *s){
  struct priv *p = s->priv;
  smbc_close(p->fd);
}

static int open_f (stream_t *stream, int mode)
{
  char *filename;
  mode_t m = 0;
  int64_t len;
  int fd, err;

  struct priv *priv = talloc_zero(stream, struct priv);
  stream->priv = priv;

  filename = stream->url;

  if(mode == STREAM_READ)
    m = O_RDONLY;
  else if (mode == STREAM_WRITE) //who's gonna do that ?
    m = O_RDWR|O_CREAT|O_TRUNC;
  else {
    mp_msg(MSGT_OPEN, MSGL_ERR, "[smb] Unknown open mode %d\n", mode);
    return STREAM_UNSUPPORTED;
  }

  if(!filename) {
    mp_msg(MSGT_OPEN,MSGL_ERR, "[smb] Bad url\n");
    return STREAM_ERROR;
  }

  err = smbc_init(smb_auth_fn, 1);
  if (err < 0) {
    mp_tmsg(MSGT_OPEN,MSGL_ERR,"Cannot init the libsmbclient library: %d\n",err);
    return STREAM_ERROR;
  }

  fd = smbc_open(filename, m,0644);
  if (fd < 0) {
    mp_tmsg(MSGT_OPEN,MSGL_ERR,"Could not open from LAN: '%s'\n", filename);
    return STREAM_ERROR;
  }

  stream->flags = mode;
  len = 0;
  if(mode == STREAM_READ) {
    len = smbc_lseek(fd,0,SEEK_END);
    smbc_lseek (fd, 0, SEEK_SET);
  }
  if(len > 0 || mode == STREAM_WRITE) {
    stream->flags |= MP_STREAM_SEEK;
    stream->seek = seek;
    if(mode == STREAM_READ) stream->end_pos = len;
  }
  priv->fd = fd;
  stream->fill_buffer = fill_buffer;
  stream->write_buffer = write_buffer;
  stream->close = close_f;
  stream->control = control;

  return STREAM_OK;
}

const stream_info_t stream_info_smb = {
  "smb",
  open_f,
  {"smb", NULL},
};
