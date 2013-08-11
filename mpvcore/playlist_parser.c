/*
 * This file is part of MPlayer.
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>

#include "talloc.h"
#include "asxparser.h"
#include "m_config.h"
#include "playlist.h"
#include "playlist_parser.h"
#include "stream/stream.h"
#include "demux/demux.h"
#include "mpvcore/mp_msg.h"
#include "mpvcore/path.h"


#define BUF_STEP 1024

#define WHITES " \n\r\t"

typedef struct play_tree_parser {
  struct stream *stream;
  char *buffer,*iter,*line;
  int buffer_size , buffer_end;
  int keep;
  struct playlist *pl;
} play_tree_parser_t;

static void
strstrip(char* str) {
  char* i;

  if (str==NULL)
    return;
  for(i = str ; i[0] != '\0' && strchr(WHITES,i[0]) != NULL; i++)
    /* NOTHING */;
  if(i[0] != '\0') {
    memmove(str,i,strlen(i) + 1);
    for(i = str + strlen(str) - 1 ; strchr(WHITES,i[0]) != NULL; i--)
      /* NOTHING */;
    i[1] = '\0';
  } else
    str[0] = '\0';
}

static char*
play_tree_parser_get_line(play_tree_parser_t* p) {
  char *end,*line_end;
  int r,resize = 0;

  if(p->buffer == NULL) {
    p->buffer = malloc(BUF_STEP);
    p->buffer_size = BUF_STEP;
    p->buffer[0] = 0;
    p->iter = p->buffer;
  }

  if(p->stream->eof && (p->buffer_end == 0 || p->iter[0] == '\0'))
    return NULL;

  assert(p->buffer_end < p->buffer_size);
  assert(!p->buffer[p->buffer_end]);
  while(1) {

    if(resize) {
      char *tmp;
      r = p->iter - p->buffer;
      end = p->buffer + p->buffer_end;
      if (p->buffer_size > INT_MAX - BUF_STEP)
        break;
      tmp = realloc(p->buffer, p->buffer_size + BUF_STEP);
      if (!tmp)
        break;
      p->buffer = tmp;
      p->iter = p->buffer + r;
      p->buffer_size += BUF_STEP;
      resize = 0;
    }

    if(p->buffer_size - p->buffer_end > 1 && ! p->stream->eof) {
      r = stream_read(p->stream,p->buffer + p->buffer_end,p->buffer_size - p->buffer_end - 1);
      if(r > 0) {
	p->buffer_end += r;
	assert(p->buffer_end < p->buffer_size);
	p->buffer[p->buffer_end] = '\0';
	while(strlen(p->buffer + p->buffer_end - r) != r)
	  p->buffer[p->buffer_end - r + strlen(p->buffer + p->buffer_end - r)] = '\n';
      }
      assert(!p->buffer[p->buffer_end]);
    }

    end = strchr(p->iter,'\n');
    if(!end) {
      if(p->stream->eof) {
	end = p->buffer + p->buffer_end;
	break;
      }
      resize = 1;
      continue;
    }
    break;
  }

  line_end = (end > p->iter && *(end-1) == '\r') ? end-1 : end;
  if(line_end - p->iter >= 0)
    p->line = realloc(p->line, line_end - p->iter + 1);
  else
    return NULL;
  if(line_end - p->iter > 0)
    strncpy(p->line,p->iter,line_end - p->iter);
  p->line[line_end - p->iter] = '\0';
  if(end[0] != '\0')
    end++;

  if(!p->keep) {
    if(end[0] != '\0') {
      p->buffer_end -= end-p->iter;
      memmove(p->buffer,end,p->buffer_end);
    } else
      p->buffer_end = 0;
    p->buffer[p->buffer_end] = '\0';
    p->iter = p->buffer;
  } else
    p->iter = end;

  return p->line;
}

static void
play_tree_parser_reset(play_tree_parser_t* p) {
  p->iter = p->buffer;
}

static void
play_tree_parser_stop_keeping(play_tree_parser_t* p) {
  p->keep = 0;
  if(p->iter && p->iter != p->buffer) {
    p->buffer_end -= p->iter -p->buffer;
    if(p->buffer_end)
      memmove(p->buffer,p->iter,p->buffer_end);
    p->buffer[p->buffer_end] = 0;
    p->iter = p->buffer;
  }
}


static bool parse_asx(play_tree_parser_t* p) {
  int comments = 0,get_line = 1;
  char* line = NULL;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying asx...\n");

  while(1) {
    if(get_line) {
      line = play_tree_parser_get_line(p);
      if(!line)
	return false;
      strstrip(line);
      if(line[0] == '\0')
	continue;
    }
    if(!comments) {
      if(line[0] != '<') {
	mp_msg(MSGT_PLAYTREE,MSGL_DBG2,"First char isn't '<' but '%c'\n",line[0]);
	mp_msg(MSGT_PLAYTREE,MSGL_DBG3,"Buffer = [%s]\n",p->buffer);
	return false;
      } else if(strncmp(line,"<!--",4) == 0) { // Comments
	comments = 1;
	line += 4;
	if(line[0] != '\0' && strlen(line) > 0)
	  get_line = 0;
      } else if(strncasecmp(line,"<ASX",4) == 0) // We got an asx element
	break;
      else // We don't get an asx
	return false;
    } else { // Comments
      char* c;
      c = strchr(line,'-');
      if(c) {
	if (strncmp(c,"--!>",4) == 0) { // End of comments
	  comments = 0;
	  line = c+4;
	  if(line[0] != '\0') // There is some more data on this line : keep it
	    get_line = 0;

	} else {
	  line = c+1; // Jump the -
	  if(line[0] != '\0') // Some more data
	    get_line = 0;
	  else  // End of line
	    get_line = 1;
	}
      } else // No - on this line (or rest of line) : get next one
	get_line = 1;
    }
  }

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Detected asx format\n");

  // We have an asx : load it in memory and parse

  while((line = play_tree_parser_get_line(p)) != NULL)
    /* NOTHING */;

 mp_msg(MSGT_PLAYTREE,MSGL_DBG3,"Parsing asx file: [%s]\n",p->buffer);
 return asx_parse(p->buffer,p->pl);
}

static char*
pls_entry_get_value(char* line) {
  char* i;

  i = strchr(line,'=');
  if(!i || i[1] == '\0')
    return NULL;
  else
    return i+1;
}

typedef struct pls_entry {
  char* file;
  char* title;
  char* length;
} pls_entry_t;

static int
pls_read_entry(char* line,pls_entry_t** _e,int* _max_entry,char** val) {
  int num,max_entry = (*_max_entry);
  pls_entry_t* e = (*_e);
  int limit = INT_MAX / sizeof(*e);
  char* v;

  v = pls_entry_get_value(line);
  if(!v) {
    mp_msg(MSGT_PLAYTREE,MSGL_ERR,"No value in entry %s\n",line);
    return -1;
  }

  num = atoi(line);
  if(num <= 0 || num > limit) {
    if (max_entry >= limit) {
        mp_msg(MSGT_PLAYTREE, MSGL_WARN, "Too many index entries\n");
        return -1;
    }
    num = max_entry+1;
    mp_msg(MSGT_PLAYTREE,MSGL_WARN,"No or invalid entry index in entry %s\nAssuming %d\n",line,num);
  }
  if(num > max_entry) {
    e = realloc(e, num * sizeof(pls_entry_t));
    if (!e)
      return -1;
    memset(&e[max_entry],0,(num-max_entry)*sizeof(pls_entry_t));
    max_entry = num;
  }
  (*_e) = e;
  (*_max_entry) = max_entry;
  (*val) = v;

  return num;
}


static bool parse_pls(play_tree_parser_t* p) {
  char *line,*v;
  pls_entry_t* entries = NULL;
  int n_entries = 0,max_entry=0,num;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying Winamp playlist...\n");
  while((line = play_tree_parser_get_line(p))) {
    strstrip(line);
    if(strlen(line))
      break;
  }
  if (!line)
    return false;
  if(strcasecmp(line,"[playlist]"))
    return false;
  mp_msg(MSGT_PLAYTREE,MSGL_V,"Detected Winamp playlist format\n");
  play_tree_parser_stop_keeping(p);
  line = play_tree_parser_get_line(p);
  if(!line)
    return false;
  strstrip(line);
  if(strncasecmp(line,"NumberOfEntries",15) == 0) {
    v = pls_entry_get_value(line);
    n_entries = atoi(v);
    if(n_entries < 0)
      mp_msg(MSGT_PLAYTREE,MSGL_DBG2,"Invalid number of entries: very funny!!!\n");
    else
      mp_msg(MSGT_PLAYTREE,MSGL_DBG2,"Playlist claims to have %d entries. Let's see.\n",n_entries);
    line = play_tree_parser_get_line(p);
  }

  while(line) {
    strstrip(line);
    if(line[0] == '\0') {
      line = play_tree_parser_get_line(p);
      continue;
    }
    if(strncasecmp(line,"File",4) == 0) {
      num = pls_read_entry(line+4,&entries,&max_entry,&v);
      if(num < 0)
	mp_msg(MSGT_PLAYTREE,MSGL_ERR,"No value in entry %s\n",line);
      else
	entries[num-1].file = strdup(v);
    } else if(strncasecmp(line,"Title",5) == 0) {
      num = pls_read_entry(line+5,&entries,&max_entry,&v);
      if(num < 0)
	mp_msg(MSGT_PLAYTREE,MSGL_ERR,"No value in entry %s\n",line);
      else
	entries[num-1].title = strdup(v);
    } else if(strncasecmp(line,"Length",6) == 0) {
      num = pls_read_entry(line+6,&entries,&max_entry,&v);
      if(num < 0)
	mp_msg(MSGT_PLAYTREE,MSGL_ERR,"No value in entry %s\n",line);
      else {
        char *end;
        long val = strtol(v, &end, 10);
        if (*end || (val <= 0 && val != -1))
          mp_msg(MSGT_PLAYTREE,MSGL_ERR,"Invalid length value in entry %s\n",line);
        else if (val > 0)
          entries[num-1].length = strdup(v);
      }
    } else
      mp_msg(MSGT_PLAYTREE,MSGL_WARN,"Unknown entry type %s\n",line);
    line = play_tree_parser_get_line(p);
  }

  for(num = 0; num < max_entry ; num++) {
    if(entries[num].file == NULL)
      mp_msg(MSGT_PLAYTREE,MSGL_ERR,"Entry %d don't have a file !!!!\n",num+1);
    else {
      mp_msg(MSGT_PLAYTREE,MSGL_DBG2,"Adding entry %s\n",entries[num].file);
      playlist_add_file(p->pl,entries[num].file);
      if (entries[num].length)
          playlist_entry_add_param(p->pl->last,  bstr0("end"), bstr0(entries[num].length));
      free(entries[num].file);
    }
    // When we have info in playtree we add these info
    free(entries[num].title);
    free(entries[num].length);
  }

  free(entries);
  return true;
}

/*
 Reference Ini-Format: Each entry is assumed a reference
 */
static bool parse_ref_ini(play_tree_parser_t* p) {
  char *line,*v;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying reference-ini playlist...\n");
  if (!(line = play_tree_parser_get_line(p)))
    return NULL;
  strstrip(line);
  if(strcasecmp(line,"[Reference]"))
    return NULL;
  mp_msg(MSGT_PLAYTREE,MSGL_V,"Detected reference-ini playlist format\n");
  play_tree_parser_stop_keeping(p);
  line = play_tree_parser_get_line(p);
  if(!line)
    return NULL;
  while(line) {
    strstrip(line);
    if(strncasecmp(line,"Ref",3) == 0) {
      v = pls_entry_get_value(line+3);
      if(!v)
	mp_msg(MSGT_PLAYTREE,MSGL_ERR,"No value in entry %s\n",line);
      else
      {
        mp_msg(MSGT_PLAYTREE,MSGL_DBG2,"Adding entry %s\n",v);
        playlist_add_file(p->pl, v);
      }
    }
    line = play_tree_parser_get_line(p);
  }

  return true;
}

static bool parse_m3u(play_tree_parser_t* p) {
  char* line;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying extended m3u playlist...\n");
  if (!(line = play_tree_parser_get_line(p)))
    return NULL;
  strstrip(line);
  if(strcasecmp(line,"#EXTM3U"))
    return NULL;
  mp_msg(MSGT_PLAYTREE,MSGL_V,"Detected extended m3u playlist format\n");
  play_tree_parser_stop_keeping(p);

  while((line = play_tree_parser_get_line(p)) != NULL) {
    strstrip(line);
    if(line[0] == '\0')
      continue;
    /* EXTM3U files contain such lines:
     * #EXTINF:<seconds>, <title>
     * followed by a line with the filename
     * for now we have no place to put that
     * so we just skip that extra-info ::atmos
     */
    if(line[0] == '#') {
#if 0 /* code functional */
      if(strncasecmp(line,"#EXTINF:",8) == 0) {
        mp_msg(MSGT_PLAYTREE,MSGL_INFO,"[M3U] Duration: %dsec  Title: %s\n",
          strtol(line+8,&line,10), line+2);
      }
#endif
      continue;
    }
    playlist_add_file(p->pl, line);
  }

  return true;
}

static bool parse_smil(play_tree_parser_t* p) {
  int entrymode=0;
  char* line,source[512],*pos,*s_start,*s_end,*src_line;
  int is_rmsmil = 0;
  unsigned int npkt, ttlpkt;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying smil playlist...\n");

  // Check if smil
  while((line = play_tree_parser_get_line(p)) != NULL) {
    strstrip(line);
    if(line[0] == '\0') // Ignore empties
      continue;
    if (strncasecmp(line,"<?xml",5)==0) // smil in xml
      continue;
    if (strncasecmp(line,"<!DOCTYPE smil",13)==0) // smil in xml
      continue;
    if (strncasecmp(line,"<smil",5)==0 || strncasecmp(line,"<?wpl",5)==0 ||
      strncasecmp(line,"(smil-document",14)==0)
      break; // smil header found
    else
      return NULL; //line not smil exit
  }

  if (!line) return NULL;
  mp_msg(MSGT_PLAYTREE,MSGL_V,"Detected smil playlist format\n");
  play_tree_parser_stop_keeping(p);

  if (strncasecmp(line,"(smil-document",14)==0) {
    mp_msg(MSGT_PLAYTREE,MSGL_V,"Special smil-over-realrtsp playlist header\n");
    is_rmsmil = 1;
    if (sscanf(line, "(smil-document (ver 1.0)(npkt %u)(ttlpkt %u", &npkt, &ttlpkt) != 2) {
      mp_msg(MSGT_PLAYTREE,MSGL_WARN,"smil-over-realrtsp: header parsing failure, assuming single packet.\n");
      npkt = ttlpkt = 1;
    }
    if (ttlpkt == 0 || npkt > ttlpkt) {
      mp_msg(MSGT_PLAYTREE,MSGL_WARN,"smil-over-realrtsp: bad packet counters (npkk = %u, ttlpkt = %u), assuming single packet.\n",
        npkt, ttlpkt);
      npkt = ttlpkt = 1;
    }
  }

  //Get entries from smil
  src_line = line;
  line = NULL;
  do {
    strstrip(src_line);
    free(line);
    line = NULL;
    /* If we're parsing smil over realrtsp and this is not the last packet and
     * this is the last line in the packet (terminating with ") ) we must get
     * the next line, strip the header, and concatenate it to the current line.
     */
    if (is_rmsmil && npkt != ttlpkt && strstr(src_line,"\")")) {
      char *payload;

      line = strdup(src_line);
      if(!(src_line = play_tree_parser_get_line(p))) {
        mp_msg(MSGT_PLAYTREE,MSGL_WARN,"smil-over-realrtsp: can't get line from packet %u/%u.\n", npkt, ttlpkt);
        break;
      }
      strstrip(src_line);
      // Skip header, packet starts after "
      if(!(payload = strchr(src_line,'\"'))) {
        mp_msg(MSGT_PLAYTREE,MSGL_WARN,"smil-over-realrtsp: can't find start of packet, using complete line.\n");
        payload = src_line;
      } else
        payload++;
      // Skip ") at the end of the last line from the current packet
      line[strlen(line)-2] = 0;
      line = realloc(line, strlen(line)+strlen(payload)+1);
      strcat (line, payload);
      npkt++;
    } else
      line = strdup(src_line);
    /* Unescape \" to " for smil-over-rtsp */
    if (is_rmsmil && line[0] != '\0') {
      int i, j;

      for (i = 0; i < strlen(line); i++)
        if (line[i] == '\\' && line[i+1] == '"')
          for (j = i; line[j]; j++)
            line[j] = line[j+1];
    }
    pos = line;
   while (pos) {
    if (!entrymode) { // all entries filled so far
     while ((pos=strchr(pos, '<'))) {
      if (strncasecmp(pos,"<video",6)==0  || strncasecmp(pos,"<audio",6)==0 || strncasecmp(pos,"<media",6)==0) {
          entrymode=1;
          break; // Got a valid tag, exit '<' search loop
      }
      pos++;
     }
    }
    if (entrymode) { //Entry found but not yet filled
      pos = strstr(pos,"src=");   // Is source present on this line
      if (pos != NULL) {
        entrymode=0;
        if (pos[4] != '"' && pos[4] != '\'') {
          mp_msg(MSGT_PLAYTREE,MSGL_V,"Unknown delimiter %c in source line %s\n", pos[4], line);
          break;
        }
        s_start=pos+5;
        s_end=strchr(s_start,pos[4]);
        if (s_end == NULL) {
          mp_msg(MSGT_PLAYTREE,MSGL_V,"Error parsing this source line %s\n",line);
          break;
        }
        if (s_end-s_start> 511) {
          mp_msg(MSGT_PLAYTREE,MSGL_V,"Cannot store such a large source %s\n",line);
          break;
        }
        strncpy(source,s_start,s_end-s_start);
        source[(s_end-s_start)]='\0'; // Null terminate
        playlist_add_file(p->pl, source);
        pos = s_end;
      }
    }
   }
  } while((src_line = play_tree_parser_get_line(p)) != NULL);

  free(line);
  return true;
}

static bool parse_textplain(play_tree_parser_t* p) {
  char* line;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying plaintext playlist...\n");
  play_tree_parser_stop_keeping(p);

  while((line = play_tree_parser_get_line(p)) != NULL) {
    strstrip(line);
    if(line[0] == '\0' || line[0] == '#' || (line[0] == '/' && line[1] == '/'))
      continue;

      playlist_add_file(p->pl,line);
  }

  return true;
}

/**
 * \brief decode the base64 used in nsc files
 * \param in input string, 0-terminated
 * \param buf output buffer, must point to memory suitable for realloc,
 *            will be NULL on failure.
 * \return decoded length in bytes
 */
static int decode_nsc_base64(char *in, char **buf) {
  int i, j, n;
  if (in[0] != '0' || in[1] != '2')
    goto err_out;
  in += 2; // skip prefix
  if (strlen(in) < 16) // error out if nothing to decode
    goto err_out;
  in += 12; // skip encoded string length
  n = strlen(in) / 4;
  *buf = realloc(*buf, n * 3);
  for (i = 0; i < n; i++) {
    uint8_t c[4];
    for (j = 0; j < 4; j++) {
      c[j] = in[4 * i + j];
      if (c[j] >= '0' && c[j] <= '9') c[j] += 0 - '0';
      else if (c[j] >= 'A' && c[j] <= 'Z') c[j] += 10 - 'A';
      else if (c[j] >= 'a' && c[j] <= 'z') c[j] += 36 - 'a';
      else if (c[j] == '{') c[j] = 62;
      else if (c[j] == '}') c[j] = 63;
      else {
        mp_msg(MSGT_PLAYTREE, MSGL_ERR, "Invalid character %c (0x%02"PRIx8")\n", c[j], c[j]);
        goto err_out;
      }
    }
    (*buf)[3 * i] = (c[0] << 2) | (c[1] >> 4);
    (*buf)[3 * i + 1] = (c[1] << 4) | (c[2] >> 2);
    (*buf)[3 * i + 2] = (c[2] << 6) | c[3];
  }
  return 3 * n;
err_out:
  free(*buf);
  *buf = NULL;
  return 0;
}

/**
 * \brief "converts" utf16 to ascii by just discarding every second byte
 * \param buf buffer to convert
 * \param len lenght of buffer, must be > 0
 */
static void utf16_to_ascii(char *buf, int len) {
  int i;
  if (len <= 0) return;
  for (i = 0; i < len / 2; i++)
    buf[i] = buf[i * 2];
  buf[i] = 0; // just in case
}

static bool parse_nsc(play_tree_parser_t* p) {
  char *line, *addr = NULL, *url, *unicast_url = NULL;
  int port = 0;

  mp_msg(MSGT_PLAYTREE,MSGL_V,"Trying nsc playlist...\n");
  while((line = play_tree_parser_get_line(p)) != NULL) {
    strstrip(line);
    if(!line[0]) // Ignore empties
      continue;
    if (strncasecmp(line,"[Address]", 9) == 0)
      break; // nsc header found
    else
      return false;
  }
  mp_msg(MSGT_PLAYTREE,MSGL_V,"Detected nsc playlist format\n");
  play_tree_parser_stop_keeping(p);
  while ((line = play_tree_parser_get_line(p)) != NULL) {
    strstrip(line);
    if (!line[0])
      continue;
    if (strncasecmp(line, "Unicast URL=", 12) == 0) {
      int len = decode_nsc_base64(&line[12], &unicast_url);
      if (len <= 0)
        mp_msg(MSGT_PLAYTREE, MSGL_WARN, "[nsc] Unsupported Unicast URL encoding\n");
      else
        utf16_to_ascii(unicast_url, len);
    } else if (strncasecmp(line, "IP Address=", 11) == 0) {
      int len = decode_nsc_base64(&line[11], &addr);
      if (len <= 0)
        mp_msg(MSGT_PLAYTREE, MSGL_WARN, "[nsc] Unsupported IP Address encoding\n");
      else
        utf16_to_ascii(addr, len);
    } else if (strncasecmp(line, "IP Port=", 8) == 0) {
      port = strtol(&line[8], NULL, 0);
    }
  }

  bool success = false;

  if (unicast_url)
    url = strdup(unicast_url);
  else if (addr && port) {
    url = malloc(strlen(addr) + 7 + 20 + 1);
    sprintf(url, "http://%s:%i", addr, port);
  } else
   goto err_out;

  playlist_add_file(p->pl, url);
  free(url);
  success = true;
err_out:
  free(addr);
  free(unicast_url);
  return success;
}

struct playlist *playlist_parse_file(const char *file)
{
  stream_t *stream = stream_open(file, NULL);
  if(!stream) {
      mp_msg(MSGT_PLAYTREE,MSGL_ERR,
             "Error while opening playlist file %s: %s\n",
             file, strerror(errno));
    return false;
  }

  mp_msg(MSGT_PLAYTREE, MSGL_V,
         "Parsing playlist file %s...\n", file);

  struct playlist *ret = playlist_parse(stream);
  free_stream(stream);

  playlist_add_base_path(ret, mp_dirname(file));

  return ret;

}

typedef bool (*parser_fn)(play_tree_parser_t *);
static const parser_fn pl_parsers[] = {
    parse_asx,
    parse_pls,
    parse_m3u,
    parse_ref_ini,
    parse_smil,
    parse_nsc,
    parse_textplain
};


static struct playlist *do_parse(struct stream* stream, bool forced)
{
  play_tree_parser_t p = {
      .stream = stream,
      .pl = talloc_zero(NULL, struct playlist),
      .keep = 1,
  };

  bool success = false;
  if (play_tree_parser_get_line(&p) != NULL) {
    for (int n = 0; n < sizeof(pl_parsers) / sizeof(pl_parsers[0]); n++) {
      play_tree_parser_reset(&p);
      if (pl_parsers[n] == parse_textplain && !forced)
        break;
      if (pl_parsers[n](&p)) {
        success = true;
        break;
      }
    }
  }

  if(success)
    mp_msg(MSGT_PLAYTREE,MSGL_V,"Playlist successfully parsed\n");
  else {
    mp_msg(MSGT_PLAYTREE,((forced==1)?MSGL_ERR:MSGL_V),"Error while parsing playlist\n");
    talloc_free(p.pl);
    p.pl = NULL;
  }

  if (p.pl && !p.pl->first)
    mp_msg(MSGT_PLAYTREE,((forced==1)?MSGL_WARN:MSGL_V),"Warning: empty playlist\n");

  return p.pl;
}

struct playlist *playlist_parse(struct stream* stream)
{
    return do_parse(stream, true);
}

struct playlist *playlist_probe_and_parse(struct stream* stream)
{
    return do_parse(stream, false);
}
