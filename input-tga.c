/* input-tga.c	reads tga files */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* #include <unistd.h> */

#include "bitmap.h"
#include "message.h"
#include "xstd.h"
#include "input-bmp.h"

/* TODO:
   - Handle loading images that aren't 8 bits per channel. 
*/

/* Round up a division to the nearest integer. */
#define ROUNDUP_DIVIDE(n,d) (((n) + (d - 1)) / (d))

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

#define INDEXED 1
#define INDEXEDA 2
#define GRAY 3
#define RGB 5
#define INDEXED_IMAGE 1
#define INDEXEDA_IMAGE 2
#define GRAY_IMAGE 3
#define GRAYA_IMAGE 4
#define RGB_IMAGE 5
#define RGBA_IMAGE 6

typedef struct _TgaSaveVals
{
  int rle;
} TgaSaveVals;

typedef struct _TgaSaveInterface
{
  int run;
} TgaSaveInterface;

struct tga_header
{
  unsigned char idLength;
  unsigned char colorMapType;

  /* The image type. */
#define TGA_TYPE_MAPPED      1
#define TGA_TYPE_COLOR       2
#define TGA_TYPE_GRAY        3
#define TGA_TYPE_MAPPED_RLE  9
#define TGA_TYPE_COLOR_RLE  10
#define TGA_TYPE_GRAY_RLE   11
  unsigned char imageType;

  /* Color Map Specification. */
  /* We need to separately specify high and low bytes to avoid endianness
     and alignment problems. */
  unsigned char colorMapIndexLo, colorMapIndexHi;
  unsigned char colorMapLengthLo, colorMapLengthHi;
  unsigned char colorMapSize;

  /* Image Specification. */
  unsigned char xOriginLo, xOriginHi;
  unsigned char yOriginLo, yOriginHi;

  unsigned char widthLo, widthHi;
  unsigned char heightLo, heightHi;

  unsigned char bpp;

  /* Image descriptor.
     3-0: attribute bpp
     4:   left-to-right ordering
     5:   top-to-bottom ordering
     7-6: zero
     */
#define TGA_DESC_ABITS      0x0f
#define TGA_DESC_HORIZONTAL 0x10
#define TGA_DESC_VERTICAL   0x20
  unsigned char descriptor;
};

static struct
{
  unsigned int extensionAreaOffset;
  unsigned int developerDirectoryOffset;
#define TGA_SIGNATURE "TRUEVISION-XFILE"
  char signature[16];
  char dot;
  char null;
} tga_footer;


static bitmap_type ReadImage (FILE *fp,
                         struct tga_header *hdr,
                         char *filename);
bitmap_type
ReadTGA (string filename)
{
  FILE *fp;
  struct tga_header hdr;

  bitmap_type image;

  image.bitmap = NULL;

  fp = fopen (filename, "rb");
  if (!fp)
      FATAL1 ("TGA: can't open \"%s\"\n", filename);

  /* Check the footer. */
  if (fseek (fp, 0L - (sizeof (tga_footer)), SEEK_END)
      || fread (&tga_footer, sizeof (tga_footer), 1, fp) != 1)
      FATAL1 ("TGA: Cannot read footer from \"%s\"\n", filename);

  /* Check the signature. */

  if (fseek (fp, 0, SEEK_SET) ||
      fread (&hdr, sizeof (hdr), 1, fp) != 1)
      FATAL1 ("TGA: Cannot read header from \"%s\"\n", filename);

  /* Skip the image ID field. */
  if (hdr.idLength && fseek (fp, hdr.idLength, SEEK_CUR))
      FATAL1 ("TGA: Cannot skip ID field in \"%s\"\n", filename);

  image = ReadImage (fp, &hdr, filename);
  fclose (fp);
  return image;
}


static int
std_fread (unsigned char *buf, 
           int     datasize, 
           int     nelems, 
           FILE   *fp)
{

  return fread (buf, datasize, nelems, fp);
}

#define RLE_PACKETSIZE 0x80

/* Decode a bufferful of file. */
static int
rle_fread (unsigned char *buf, 
           int     datasize, 
           int     nelems, 
           FILE   *fp)
{
  static unsigned char *statebuf = 0;
  static int statelen = 0;
  static int laststate = 0;

  int j, k;
  int buflen, count, bytes;
  unsigned char *p;

  /* Scale the buffer length. */
  buflen = nelems * datasize;

  j = 0;
  while (j < buflen)
    {
      if (laststate < statelen)
        {
          /* Copy bytes from our previously decoded buffer. */
          bytes = MIN (buflen - j, statelen - laststate);
          memcpy (buf + j, statebuf + laststate, bytes);
          j += bytes;
          laststate += bytes;

          /* If we used up all of our state bytes, then reset them. */
          if (laststate >= statelen)
            {
              laststate = 0;
              statelen = 0;
            }

          /* If we filled the buffer, then exit the loop. */
          if (j >= buflen)
            break;
        }

      /* Decode the next packet. */
      count = fgetc (fp);
      if (count == EOF)
        {
	    return j / datasize;
        }

      /* Scale the byte length to the size of the data. */
      bytes = ((count & ~RLE_PACKETSIZE) + 1) * datasize;

      if (j + bytes <= buflen)
        {
          /* We can copy directly into the image buffer. */
          p = buf + j;
        }
      else {
	  /* Allocate the state buffer if we haven't already. */
        if (!statebuf)
          statebuf = (unsigned char *) malloc (RLE_PACKETSIZE * datasize);
        p = statebuf;
      }

      if (count & RLE_PACKETSIZE)
        {
          /* Fill the buffer with the next value. */
          if (fread (p, datasize, 1, fp) != 1)
            {
		  				return j / datasize;
            }

          /* Optimized case for single-byte encoded data. */
          if (datasize == 1)
            memset (p + 1, *p, bytes - 1);
          else
            for (k = datasize; k < bytes; k += datasize)
              memcpy (p + k, p, datasize);
        }
      else
        {
          /* Read in the buffer. */
          if (fread (p, bytes, 1, fp) != 1)
            {
	       return j / datasize;
            }
        }

      /* We may need to copy bytes from the state buffer. */
      if (p == statebuf)
        statelen = bytes;
      else
        j += bytes;
    }

return nelems;
}

static bitmap_type
ReadImage (FILE              *fp, 
           struct tga_header *hdr, 
           char              *filename)
{
  bitmap_type image;
  unsigned char *buffer;
  unsigned char *alphas;

  int width, height, bpp, abpp, pbpp, nalphas;
  int j, k;
  int pelbytes, wbytes, bsize, npels, pels;
  int rle, badread;
  int itype, dtype;
  unsigned char *cmap = NULL;
  int (*myfread)(unsigned char *, int, int, FILE *);

  /* Find out whether the image is horizontally or vertically reversed. */
  char horzrev = hdr->descriptor & TGA_DESC_HORIZONTAL;
  char vertrev = !(hdr->descriptor & TGA_DESC_VERTICAL);

  image.bitmap = NULL;
  
  /* Reassemble the multi-byte values correctly, regardless of
     host endianness. */
  width = (hdr->widthHi << 8) | hdr->widthLo;
  height = (hdr->heightHi << 8) | hdr->heightLo;

  bpp = hdr->bpp;
  abpp = hdr->descriptor & TGA_DESC_ABITS;

  if (hdr->imageType == TGA_TYPE_COLOR ||
      hdr->imageType == TGA_TYPE_COLOR_RLE)
    pbpp = MIN (bpp / 3, 8) * 3;
  else if (abpp < bpp)
    pbpp = bpp - abpp;
  else
    pbpp = bpp;

  if (abpp + pbpp > bpp)
    {
      WARNING3 ("TGA: %d bit image, %d bit alpha is greater than %d total bits per pixel\n",
              pbpp, abpp, bpp);

      /* Assume that alpha bits were set incorrectly. */
      abpp = bpp - pbpp;
      WARNING1 ("TGA: reducing to %d bit alpha\n", abpp);
    }
  else if (abpp + pbpp < bpp)
    {
      WARNING3 ("TGA: %d bit image, %d bit alpha is less than %d total bits per pixel\n",
              pbpp, abpp, bpp);

      /* Again, assume that alpha bits were set incorrectly. */
      abpp = bpp - pbpp;
      WARNING1 ("TGA: increasing to %d bit alpha\n", abpp);
    }

  rle = 0;
  switch (hdr->imageType)
    {
    case TGA_TYPE_MAPPED_RLE:
      rle = 1;
    case TGA_TYPE_MAPPED:
      itype = INDEXED;

      /* Find the size of palette elements. */
      pbpp = MIN (hdr->colorMapSize / 3, 8) * 3;
      if (pbpp < hdr->colorMapSize)
        abpp = hdr->colorMapSize - pbpp;
      else
        abpp = 0;


      if (bpp != 8)
	    /* We can only cope with 8-bit indices. */
          FATAL ("TGA: index sizes other than 8 bits are unimplemented\n");

      if (abpp)
        dtype = INDEXEDA_IMAGE;
      else
        dtype = INDEXED_IMAGE;
      break;

    case TGA_TYPE_GRAY_RLE:
      rle = 1;
    case TGA_TYPE_GRAY:
      itype = GRAY;

      if (abpp)
        dtype = GRAYA_IMAGE;
      else
        dtype = GRAY_IMAGE;
      break;

    case TGA_TYPE_COLOR_RLE:
      rle = 1;
    case TGA_TYPE_COLOR:
      itype = RGB;

      if (abpp)
        dtype = RGBA_IMAGE;
      else
        dtype = RGB_IMAGE;
      break;

    default:
      FATAL1 ("TGA: unrecognized image type %d\n", hdr->imageType);
}

  if ((abpp && abpp != 8) ||
      ((itype == RGB || itype == INDEXED) && pbpp != 24) ||
      (itype == GRAY && pbpp != 8))
      /* FIXME: We haven't implemented bit-packed fields yet. */
      FATAL ("TGA: channel sizes other than 8 bits are unimplemented\n");

  /* Check that we have a color map only when we need it. */
  if (itype == INDEXED)
    {
      if (hdr->colorMapType != 1)
	    FATAL1 ("TGA: indexed image has invalid color map type %d\n",
                  hdr->colorMapType);
    }
  else if (hdr->colorMapType != 0)
      FATAL1 ("TGA: non-indexed image has invalid color map type %d\n",
              hdr->colorMapType);

  alphas = 0;
  nalphas = 0;
  if (hdr->colorMapType == 1)
    {
      /* We need to read in the colormap. */
      int index, colors;
	  unsigned int length;
      int tmp;

      index = (hdr->colorMapIndexHi << 8) | hdr->colorMapIndexLo;
      length = (hdr->colorMapLengthHi << 8) | hdr->colorMapLengthLo;

	  if (length == 0)
        FATAL1 ("TGA: invalid color map length %d\n", length);

      pelbytes = ROUNDUP_DIVIDE (hdr->colorMapSize, 8);
      colors = length + index;
      cmap = (unsigned char *) malloc (colors * pelbytes);

      /* Zero the entries up to the beginning of the map. */
      memset (cmap, 0, index * pelbytes);

      /* Read in the rest of the colormap. */
      if (fread (cmap + (index * pelbytes), pelbytes, length, fp) != length)
        FATAL1 ("TGA: error reading colormap (ftell == %ld)\n", ftell (fp));

      /* If we have an alpha channel, then create a mapping to the alpha
         values. */
      if (pelbytes > 3)
        alphas = (unsigned char *) malloc (colors);

      k = 0;
      for (j = 0; j < colors * pelbytes; j += pelbytes)
        {
          /* Swap from BGR to RGB. */
          tmp = cmap[j];
          cmap[k ++] = cmap[j + 2];
          cmap[k ++] = cmap[j + 1];
          cmap[k ++] = tmp;

          /* Take the alpha values out of the colormap. */
          if (alphas)
            alphas[nalphas ++] = cmap[j + 3];
        }

      /* If the last color was transparent, then omit it from the
         mapping. */
      if (nalphas && alphas[nalphas - 1] == 0)
        colors --;
 
      /* Now pretend as if we only have 8 bpp. */
      abpp = 0;
      pbpp = 8;
    }
 
  /* Calculate number of bytes per pixel. */
  pelbytes = 3;

  image.bitmap = (unsigned char *) malloc (width * height * 3 * sizeof(unsigned char));
  BITMAP_WIDTH (image) = width;
  BITMAP_HEIGHT (image) = height;
  BITMAP_PLANES (image) = 3;

   /* Calculate TGA bytes per pixel. */
  bpp = ROUNDUP_DIVIDE (pbpp + abpp, 8);
 
  /* Maybe we need to reverse the data. */
  buffer = NULL;
  if (horzrev || vertrev)
    buffer = (unsigned char *) malloc (width * height * pelbytes * sizeof (char));  
  if (rle)
    myfread = rle_fread;        
  else
    myfread = std_fread;
 
  wbytes = width * pelbytes;
  badread = 0;

  npels = width * height;
  bsize = wbytes * height;
 
  /* Suck in the data one height at a time. */
  if (badread)
    pels = 0;
  else
    pels = (*myfread) (image.bitmap, bpp, npels, fp);
 
  if (pels != npels)
    {
      if (!badread)
        {
          /* Probably premature end of file. */
          WARNING1 ("TGA: error reading (ftell == %ld)\n", ftell (fp));
          badread = 1;
        }
 

      /* Fill the rest of this tile with zeros. */
      memset (image.bitmap + (pels * bpp), 0, ((npels - pels) * bpp));
    }
  /* If we have indexed alphas, then set them. */
  if (nalphas)
    {
      /* Start at the end of the buffer, and work backwards. */
      k = (npels - 1) * bpp;
      for (j = bsize - pelbytes; j >= 0; j -= pelbytes)
        {
          /* Find the alpha for this index. */
          image.bitmap[j + 1] = alphas[image.bitmap[k]];
          image.bitmap[j] = image.bitmap[k --];
        }
    }

   if (itype == GRAY)
      for (j = bsize/3 - 1; j >= 0; j -= 1)
        {
          /* Find the alpha for this index. */
          image.bitmap[3*j] = image.bitmap[j];
          image.bitmap[3*j+1] = image.bitmap[j];
          image.bitmap[3*j+2] = image.bitmap[j];
        }


  if (pelbytes >= 3)
    {
      /* Rearrange the colors from BGR to RGB. */
      int tmp;
      for (j = 0; j < bsize; j += pelbytes)
        {
          tmp = image.bitmap[j];
          image.bitmap[j] = image.bitmap[j + 2];
          image.bitmap[j + 2] = tmp;
        }
    }
 
 
  if (horzrev || vertrev)
    {
      unsigned char *tmp;
      if (vertrev)
        {
          /* We need to mirror only vertically. */
          for (j = 0; j < bsize; j += wbytes)
            memcpy (buffer + j,
              image.bitmap + bsize - (j + wbytes), wbytes);
        }
      else if (horzrev)
        {
          /* We need to mirror only horizontally. */
          for (j = 0; j < bsize; j += wbytes)
            for (k = 0; k < wbytes; k += pelbytes)
              memcpy (buffer + k + j,
                image.bitmap + (j + wbytes) - (k + pelbytes), pelbytes);
        }
      else
        {
          /* Completely reverse the pixels in the buffer. */
          for (j = 0; j < bsize; j += pelbytes)
            memcpy (buffer + j,
              image.bitmap + bsize - (j + pelbytes), pelbytes);
        }
  
       /* Swap the buffers because we modified them. */
      tmp = buffer;
      buffer = image.bitmap;
      image.bitmap = tmp;
    }

  if (fgetc (fp) != EOF)
    WARNING ("TGA: too much input data, ignoring extra...\n");

  free (buffer);

  if (hdr->colorMapType == 1)
    {
      unsigned char *temp, *temp2, *temp3;
      unsigned char index;
      int xpos, ypos;

      temp2 = temp = image.bitmap;
      image.bitmap = temp3 = (unsigned char *) malloc (width * height * 3 * sizeof (unsigned char));
	  
      for (ypos = 0; ypos < height; ypos++)
        {
          for (xpos = 0; xpos < width; xpos++)
            {
               index = *temp2++;
               *temp3++ = cmap[3*index+0];
               *temp3++ = cmap[3*index+1];
               *temp3++ = cmap[3*index+2];
	    }
        }
      free (temp);
      free (cmap);
    }
 
  if (alphas)
    free (alphas);
 
  return image;
}  /* read_image */

/* version 0.xx */
