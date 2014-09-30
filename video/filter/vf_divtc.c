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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#include <libavutil/bswap.h>

#include "config.h"
#include "common/msg.h"
#include "options/m_option.h"

#include "video/img_format.h"
#include "video/mp_image.h"
#include "vf.h"

#include "video/memcpy_pic.h"

const vf_info_t vf_info_divtc;

struct vf_priv_s
   {
   int deghost, pass, phase, window, fcount, bcount, frameno, misscount,
      ocount, sum[5];
   double threshold;
   char *filename;
   FILE *file;
   int8_t *bdata;
   unsigned int *csdata;
   int *history;
   struct vf_detc_pts_buf ptsbuf;
   struct mp_image *buffer;
   };

static int diff(unsigned char *old, unsigned char *new, int os, int ns)
   {
   int x, y, d=0;

   for(y=8; y; y--, new+=ns, old+=os)
      for(x=8; x; x--)
         d+=abs(new[x]-old[x]);

   return d;
   }

static int diff_plane(unsigned char *old, unsigned char *new,
                      int w, int h, int os, int ns, int arg)
   {
   int x, y, d, max=0, sum=0, n=0;

   for(y=0; y<h-7; y+=8)
      {
      for(x=0; x<w-7; x+=8)
         {
         d=diff(old+x+y*os, new+x+y*ns, os, ns);
         if(d>max) max=d;
         sum+=d;
         n++;
         }
      }

   return (sum+n*max)/2;
   }

/*
static unsigned int checksum_plane(unsigned char *p, unsigned char *z,
                                   int w, int h, int s, int zs, int arg)
   {
   unsigned int shift, sum;
   unsigned char *e;

   for(sum=0; h; h--, p+=s-w)
      for(e=p+w, shift=32; p<e;)
         sum^=(*p++)<<(shift=(shift-8)&31);

   return sum;
   }
*/

#define FAST_64BIT (UINTPTR_MAX >= UINT64_MAX)

static unsigned int checksum_plane(unsigned char *p, unsigned char *z,
                                   int w, int h, int s, int zs, int arg)
   {
   unsigned int shift;
   uint32_t sum, t;
   unsigned char *e, *e2;
#if FAST_64BIT
   typedef uint64_t wsum_t;
#else
   typedef uint32_t wsum_t;
#endif
   wsum_t wsum;

   for(sum=0; h; h--, p+=s-w)
      {
      for(shift=0, e=p+w; (size_t)p&(sizeof(wsum_t)-1) && p<e;)
         sum^=*p++<<(shift=(shift-8)&31);

      for(wsum=0, e2=e-sizeof(wsum_t)+1; p<e2; p+=sizeof(wsum_t))
         wsum^=*(wsum_t *)p;

#if FAST_64BIT
      t=av_be2ne32((uint32_t)(wsum>>32^wsum));
#else
      t=av_be2ne32(wsum);
#endif

      for(sum^=(t<<shift|t>>(32-shift)); p<e;)
         sum^=*p++<<(shift=(shift-8)&31);
      }

   return sum;
   }

static int deghost_plane(unsigned char *d, unsigned char *s,
                         int w, int h, int ds, int ss, int threshold)
   {
   int t;
   unsigned char *e;

   for(; h; h--, s+=ss-w, d+=ds-w)
      for(e=d+w; d<e; d++, s++)
         if(abs(*d-*s)>=threshold)
            *d=(t=(*d<<1)-*s)<0?0:t>255?255:t;

   return 0;
   }

static int copyop(unsigned char *d, unsigned char *s, int bpl, int h, int dstride, int sstride, int dummy) {
  memcpy_pic(d, s, bpl, h, dstride, sstride);
  return 0;
}

static int imgop(int(*planeop)(unsigned char *, unsigned char *,
                               int, int, int, int, int),
                 mp_image_t *dst, mp_image_t *src, int arg)
   {
       int sum = 0;
       for (int p = 0; p < dst->num_planes; p++) {
           sum += planeop(dst->planes[p], src ? src->planes[p] : NULL,
                          (dst->w * dst->fmt.bytes[p]) >> dst->fmt.xs[p],
                          dst->plane_h[p], dst->stride[p],
                          src ? src->stride[p] : 0, arg);
       }
       return sum;
   }

/*
 * Find the phase in which the telecine pattern fits best to the
 * given 5 frame slice of frame difference measurements.
 *
 * If phase1 and phase2 are not negative, only the two specified
 * phases are tested.
 */

static int match(struct vf_priv_s *p, int *diffs,
                 int phase1, int phase2, double *strength)
   {
   const int pattern1[]={ -4,  1, 1, 1, 1 },
      pattern2[]={ -2, -3, 4, 4, -3 }, *pattern;
   int f, m, n, t[5];

   pattern=p->deghost>0?pattern2:pattern1;

   for(f=0; f<5; f++)
      {
      if(phase1<0 || phase2<0 || f==phase1 || f==phase2)
         {
         for(n=t[f]=0; n<5; n++)
            t[f]+=diffs[n]*pattern[(n-f+5)%5];
         }
      else
         t[f]=INT_MIN;
      }

   /* find the best match */
   for(m=0, n=1; n<5; n++)
      if(t[n]>t[m]) m=n;

   if(strength)
      {
      /* the second best match */
      for(f=m?0:1, n=f+1; n<5; n++)
         if(n!=m && t[n]>t[f]) f=n;

      *strength=(t[m]>0?(double)(t[m]-t[f])/t[m]:0.0);
      }

   return m;
   }

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi)
   {
   int n, m, f, newphase;
   struct vf_priv_s *p=vf->priv;
   unsigned int checksum;
   double d;

   if (!p->buffer || p->buffer->w != mpi->w || p->buffer->h != mpi->h ||
       p->buffer->imgfmt != mpi->imgfmt)
   {
       mp_image_unrefp(&p->buffer);
       p->buffer = mp_image_alloc(mpi->imgfmt, mpi->w, mpi->h);
       talloc_steal(vf, p->buffer);
       if (!p->buffer)
           return NULL; // skip on OOM
   }

   struct mp_image *dmpi = p->buffer;
   double pts = mpi->pts;

   newphase=p->phase;

   switch(p->pass)
      {
      case 1:
         fprintf(p->file, "%08x %d\n",
                 (unsigned int)imgop((void *)checksum_plane, mpi, 0, 0),
                 p->frameno?imgop(diff_plane, dmpi, mpi, 0):0);
         break;

      case 2:
         if(p->frameno/5>p->bcount)
            {
            MP_ERR(vf, "\n%s: Log file ends prematurely! "
                   "Switching to one pass mode.\n", vf->info->name);
            p->pass=0;
            break;
            }

         checksum=(unsigned int)imgop((void *)checksum_plane, mpi, 0, 0);

         if(checksum!=p->csdata[p->frameno])
            {
            for(f=0; f<100; f++)
               if(p->frameno+f<p->fcount && p->csdata[p->frameno+f]==checksum)
                  break;
               else if(p->frameno-f>=0 && p->csdata[p->frameno-f]==checksum)
                  {
                  f=-f;
                  break;
                  }

            if(f<100)
               {
               MP_INFO(vf, "\n%s: Mismatch with pass-1: %+d frame(s).\n",
                      vf->info->name, f);

               p->frameno+=f;
               p->misscount=0;
               }
            else if(p->misscount++>=30)
               {
               MP_ERR(vf, "\n%s: Sync with pass-1 lost! "
                      "Switching to one pass mode.\n", vf->info->name);
               p->pass=0;
               break;
               }
            }

         n=(p->frameno)/5;
         if(n>=p->bcount) n=p->bcount-1;

         newphase=p->bdata[n];
         break;

      default:
         if(p->frameno)
            {
            int *sump=p->sum+p->frameno%5,
               *histp=p->history+p->frameno%p->window;

            *sump-=*histp;
            *sump+=(*histp=imgop(diff_plane, dmpi, mpi, 0));
            }

         m=match(p, p->sum, -1, -1, &d);

         if(d>=p->threshold)
            newphase=m;
      }

   n=p->ocount++%5;

   if(newphase!=p->phase && ((p->phase+4)%5<n)==((newphase+4)%5<n))
      {
      p->phase=newphase;
      MP_INFO(vf, "\n%s: Telecine phase %d.\n", vf->info->name, p->phase);
      }

   switch((p->frameno++-p->phase+10)%5)
      {
      case 0:
         imgop(copyop, dmpi, mpi, 0);
         vf_detc_adjust_pts(&p->ptsbuf, pts, 0, 1);
         talloc_free(mpi);
         return 0;

      case 4:
         if(p->deghost>0)
            {
            imgop(copyop, dmpi, mpi, 0);
            if (!vf_make_out_image_writeable(vf, mpi))
                return NULL; // oom: eof

            imgop(deghost_plane, mpi, dmpi, p->deghost);
            mpi->pts = vf_detc_adjust_pts(&p->ptsbuf, pts, 0, 0);
            return mpi;
            }
      }

   imgop(copyop, dmpi, mpi, 0);
   mpi->pts = vf_detc_adjust_pts(&p->ptsbuf, pts, 0, 0);
   return mpi;
   }

static int analyze(struct vf_instance *vf)
   {
   struct vf_priv_s *p = vf->priv;
   int *buf=0, *bp, bufsize=0, n, b, f, i, j, m, s;
   unsigned int *cbuf=0, *cp;
   int8_t *pbuf;
   int8_t lbuf[256];
   int sum[5];
   double d;

   /* read the file */

   n=15;
   while(fgets(lbuf, 256, p->file))
      {
      if(n>=bufsize-19)
         {
         bufsize=bufsize?bufsize*2:30000;
         if((bp=realloc(buf, bufsize*sizeof *buf))) buf=bp;
         if((cp=realloc(cbuf, bufsize*sizeof *cbuf))) cbuf=cp;

         if(!bp || !cp)
            {
            MP_FATAL(vf, "%s: Not enough memory.\n",
                   vf_info_divtc.name);
            free(buf);
            free(cbuf);
            return 0;
            }
         }
      sscanf(lbuf, "%x %d", cbuf+n, buf+n);
      n++;
      }

   if(n <= 15)
      {
      MP_FATAL(vf, "%s: Empty 2-pass log file.\n",
             vf_info_divtc.name);
      free(buf);
      free(cbuf);
      return 0;
      }

   /* generate some dummy data past the beginning and end of the array */

   buf+=15, cbuf+=15;
   n-=15;

   memcpy(buf-15, buf, 15*sizeof *buf);
   memset(cbuf-15, 0, 15*sizeof *cbuf);

   while(n%5)
      buf[n]=buf[n-5], cbuf[n]=0, n++;

   memcpy(buf+n, buf+n-15, 15*sizeof *buf);
   memset(cbuf+n, 0, 15*sizeof *cbuf);

   p->csdata=cbuf;
   p->fcount=n;

   /* array with one slot for each slice of 5 frames */

   p->bdata=pbuf=malloc(p->bcount=b=(n/5));
   memset(pbuf, 255, b);

   /* resolve the automatic mode */

   if(p->deghost<0)
      {
      int deghost=-p->deghost;
      double s0=0.0, s1=0.0;

      for(f=0; f<n; f+=5)
         {
         p->deghost=0; match(p, buf+f, -1, -1, &d); s0+=d;
         p->deghost=1; match(p, buf+f, -1, -1, &d); s1+=d;
         }

      p->deghost=s1>s0?deghost:0;

      MP_INFO(vf, "%s: Deghosting %-3s (relative pattern strength %+.2fdB).\n",
             vf_info_divtc.name,
             p->deghost?"ON":"OFF",
             10.0*log10(s1/s0));
      }

   /* analyze the data */

   for(f=0; f<5; f++)
      for(sum[f]=0, n=-15; n<20; n+=5)
         sum[f]+=buf[n+f];

   for(f=0; f<b; f++)
      {
      m=match(p, sum, -1, -1, &d);

      if(d>=p->threshold)
         pbuf[f]=m;

      if(f<b-1)
         for(n=0; n<5; n++)
            sum[n]=sum[n]-buf[5*(f-3)+n]+buf[5*(f+4)+n];
      }

   /* fill in the gaps */

   /* the beginning */
   for(f=0; f<b && pbuf[f]==-1; f++);

   if(f==b)
      {
      free(buf-15);
      MP_FATAL(vf, "%s: No telecine pattern found!\n",
             vf_info_divtc.name);
      return 0;
      }

   for(n=0; n<f; pbuf[n++]=pbuf[f]);

   /* the end */
   for(f=b-1; pbuf[f]==-1; f--);
   for(n=f+1; n<b; pbuf[n++]=pbuf[f]);

   /* the rest */
   for(f=0;;)
      {
      while(f<b && pbuf[f]!=-1) f++;
      if(f==b) break;
      for(n=f; pbuf[n]==-1; n++);

      if(pbuf[f-1]==pbuf[n])
         {
         /* just a gap */
         while(f<n) pbuf[f++]=pbuf[n];
         }
      else
         {
         /* phase change, reanalyze the original data in the gap with zero
            threshold for only the two phases that appear at the ends */

         for(i=0; i<5; i++)
            for(sum[i]=0, j=5*f-15; j<5*f; j+=5)
               sum[i]+=buf[i+j];

         for(i=f; i<n; i++)
            {
            pbuf[i]=match(p, sum, pbuf[f-1], pbuf[n], 0);

            for(j=0; j<5; j++)
               sum[j]=sum[j]-buf[5*(i-3)+j]+buf[5*(i+4)+j];
            }

         /* estimate the transition point by dividing the gap
            in the same proportion as the number of matches of each kind */

         for(i=f, m=f; i<n; i++)
            if(pbuf[i]==pbuf[f-1]) m++;

         /* find the transition of the right direction nearest to the
            estimated point */

         if(m>f && m<n)
            {
            for(j=m; j>f; j--)
               if(pbuf[j-1]==pbuf[f-1] && pbuf[j]==pbuf[n]) break;
            for(s=m; s<n; s++)
               if(pbuf[s-1]==pbuf[f-1] && pbuf[s]==pbuf[n]) break;

            m=(s-m<m-j)?s:j;
            }

         /* and rewrite the data to allow only this one transition */

         for(i=f; i<m; i++)
            pbuf[i]=pbuf[f-1];

         for(; i<n; i++)
            pbuf[i]=pbuf[n];

         f=n;
         }
      }

   free(buf-15);

   return 1;
   }

static int query_format(struct vf_instance *vf, unsigned int fmt)
   {
   switch(fmt)
      {
      case IMGFMT_444P: case IMGFMT_RGB24:
      case IMGFMT_422P: case IMGFMT_UYVY: case IMGFMT_BGR24:
      case IMGFMT_411P: case IMGFMT_YUYV: case IMGFMT_410P:
      case IMGFMT_420P: case IMGFMT_Y8:
         return vf_next_query_format(vf,fmt);
      }

   return 0;
   }

static void uninit(struct vf_instance *vf)
   {
   if(vf->priv)
      {
      if(vf->priv->file) fclose(vf->priv->file);
      if(vf->priv->csdata) free(vf->priv->csdata-15);
      free(vf->priv->bdata);
      free(vf->priv->history);
      }
   }

static int control(vf_instance_t *vf, int request, void *data)
{
    switch (request) {
    case VFCTRL_SEEK_RESET:
        vf_detc_init_pts_buf(&vf->priv->ptsbuf);
        return CONTROL_OK;
    }
    return CONTROL_UNKNOWN;
}

static int vf_open(vf_instance_t *vf)
   {
   struct vf_priv_s *p = vf->priv;

   vf->filter=filter;
   vf->uninit=uninit;
   vf->query_format=query_format;
   vf->control=control;

   p->window=5*(p->window+4)/5;

   if (!p->filename)
       p->pass = 0;

      switch(p->pass)
      {
      case 1:
         if(!(p->file=fopen(p->filename, "w")))
            {
            MP_FATAL(vf, "%s: Can't create file %s.\n", vf->info->name, p->filename);
            goto fail;
            }

         break;

      case 2:
         if(!(p->file=fopen(p->filename, "r")))
            {
            MP_FATAL(vf, "%s: Can't open file %s.\n", vf->info->name, p->filename);
            goto fail;
            }

         if(!analyze(vf))
            goto fail;

         fclose(p->file);
         p->file=0;
         break;
      }

   if(!(p->history=calloc(sizeof *p->history, p->window)))
      abort();

   vf_detc_init_pts_buf(&p->ptsbuf);
   return 1;
   fail:
   uninit(vf);
   return 0;
   }

#define OPT_BASE_STRUCT struct vf_priv_s
const vf_info_t vf_info_divtc = {
    .description = "inverse telecine for deinterlaced video",
    .name = "divtc",
    .open = vf_open,
    .priv_size = sizeof(struct vf_priv_s),
    .priv_defaults = &(const struct vf_priv_s){
        .phase = 5,
        .threshold = 0.5,
        .window = 30,
    },
    .options = (const struct m_option[]){
        OPT_INTRANGE("phase", phase, 0, 0, 4),
        OPT_INTRANGE("pass", pass, 0, 0, 2),
        OPT_DOUBLE("threshold", threshold, 0),
        OPT_INTRANGE("window", window, 0, 5, 9999),
        OPT_INT("deghost", deghost, 0),
        OPT_STRING("file", filename, 0),
        {0}
    },
};
