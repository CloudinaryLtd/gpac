/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean le Feuvre
 *				Copyright (c) 2005-200X ENST
 *					All rights reserved
 *
 *  This file is part of GPAC / LASeR codec sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#include <gpac/internal/laser_dev.h>
#include <gpac/bitstream.h>
#include <gpac/math.h>

static void lsr_dec_log_bits(GF_LASeRCodec *lsr, u32 val, u32 nb_bits, const char *name)
{
	if (!lsr->trace) return;
	fprintf(lsr->trace, "%s\t\t%d\t\t%d", name, nb_bits, val);
	fprintf(lsr->trace, "\n");
	fflush(lsr->trace);
}

#define GF_LSR_READ_INT(_codec, _val, _nbBits, _str)	{\
	_val = gf_bs_read_int(_codec->bs, _nbBits);	\
	lsr_dec_log_bits(_codec, _val, _nbBits, _str); \
	}\


static void lsr_read_group_content(GF_LASeRCodec *lsr, SVGElement *elt, Bool skip_object_content);
static GF_Err lsr_read_command_list(GF_LASeRCodec *lsr, GF_List *comList, SVGscriptElement *script);
static GF_Err lsr_decode_laser_unit(GF_LASeRCodec *lsr, GF_List *com_list);

GF_LASeRCodec *gf_laser_decoder_new(GF_SceneGraph *graph)
{
	GF_LASeRCodec *tmp;
	GF_SAFEALLOC(tmp, sizeof(GF_LASeRCodec));
	if (!tmp) return NULL;
	tmp->streamInfo = gf_list_new();
	tmp->font_table = gf_list_new();
	tmp->sg = graph;
	return tmp;
}

void gf_laser_decoder_del(GF_LASeRCodec *codec)
{	
	/*destroy all config*/
	while (gf_list_count(codec->streamInfo)) {
		LASeRStreamInfo *p = gf_list_last(codec->streamInfo);
		free(p);
		gf_list_rem_last(codec->streamInfo);
	}
	gf_list_del(codec->streamInfo);
	if (codec->col_table) free(codec->col_table);
	while (gf_list_count(codec->font_table)) {
		char *ft = gf_list_last(codec->font_table);
		free(ft);
		gf_list_rem_last(codec->font_table);
	}
	gf_list_del(codec->font_table);
	free(codec);
}


void gf_laser_decoder_set_trace(GF_LASeRCodec *codec, FILE *trace)
{
	codec->trace = trace;
	if (trace) fprintf(codec->trace, "Name\t\tNbBits\t\tValue\t\t//comment\n\n");
}

static LASeRStreamInfo *lsr_get_stream(GF_LASeRCodec *codec, u16 ESID)
{
	u32 i;
	for (i=0;i<gf_list_count(codec->streamInfo);i++) {
		LASeRStreamInfo *ptr = gf_list_get(codec->streamInfo, i);
		if(ptr->ESID==ESID) return ptr;
	}
	return NULL;
}


GF_Err gf_laser_decoder_configure_stream(GF_LASeRCodec *codec, u16 ESID, char *dsi, u32 dsi_len)
{
	LASeRStreamInfo *info;
	GF_BitStream *bs;
	if (lsr_get_stream(codec, ESID) != NULL) return GF_BAD_PARAM;
	GF_SAFEALLOC(info, sizeof(LASeRStreamInfo));
	info->ESID = ESID;
	bs = gf_bs_new(dsi, dsi_len, GF_BITSTREAM_READ);

	info->cfg.profile = gf_bs_read_int(bs, 8);
	info->cfg.level = gf_bs_read_int(bs, 8);
	info->cfg.encoding = gf_bs_read_int(bs, 2);
	info->cfg.pointsCodec = gf_bs_read_int(bs, 2);
	info->cfg.pathComponents = gf_bs_read_int(bs, 8);
	info->cfg.fullRequestHost = gf_bs_read_int(bs, 1);
	if (gf_bs_read_int(bs, 1)) {
		info->cfg.time_resolution = gf_bs_read_int(bs, 16);
	} else {
		info->cfg.time_resolution = 1000;
	}
	info->cfg.colorComponentBits = gf_bs_read_int(bs, 4);
	info->cfg.colorComponentBits += 1;
	info->cfg.resolution = gf_bs_read_int(bs, 4);
	if (info->cfg.resolution>7) info->cfg.resolution -= 16;
	info->cfg.scale_bits = gf_bs_read_int(bs, 4);
	info->cfg.coord_bits = gf_bs_read_int(bs, 5);
	info->cfg.append = gf_bs_read_int(bs, 1);
	info->cfg.has_string_ids = gf_bs_read_int(bs, 1);
	info->cfg.has_private_data = gf_bs_read_int(bs, 1);
	info->cfg.hasExtendedAttributes = gf_bs_read_int(bs, 1);
	info->cfg.extensionIDBits = gf_bs_read_int(bs, 4);
	gf_list_add(codec->streamInfo, info);
	gf_bs_del(bs);
	return GF_OK;
}

GF_Err gf_laser_decoder_remove_stream(GF_LASeRCodec *codec, u16 ESID)
{
	u32 i;
	for (i=0;i<gf_list_count(codec->streamInfo);i++) {
		LASeRStreamInfo *ptr = (LASeRStreamInfo *) gf_list_get(codec->streamInfo, i);
		if (ptr->ESID==ESID) {
			free(ptr);
			gf_list_rem(codec->streamInfo, i);
			return GF_OK;
		}
	}
	return GF_BAD_PARAM;
}



GF_Err gf_laser_decode_au(GF_LASeRCodec *codec, u16 ESID, char *data, u32 data_len)
{
	GF_Err e;
	if (!codec || !data || !data_len) return GF_BAD_PARAM;

	codec->info = lsr_get_stream(codec, ESID);
	if (!codec->info) return GF_BAD_PARAM;
	codec->coord_bits = codec->info->cfg.coord_bits;
	codec->scale_bits = codec->info->cfg.scale_bits;
	codec->time_resolution = codec->info->cfg.time_resolution;
	codec->color_scale = (1<<codec->info->cfg.colorComponentBits) - 1;
	if (codec->info->cfg.resolution>=0)
		codec->res_factor = INT2FIX(1<<codec->info->cfg.resolution);
	else 
		codec->res_factor = gf_divfix(FIX_ONE, INT2FIX(1 << (-codec->info->cfg.resolution)) );

	codec->bs = gf_bs_new(data, data_len, GF_BITSTREAM_READ);
	e = lsr_decode_laser_unit(codec, NULL);
	gf_bs_del(codec->bs);
	codec->bs = NULL;
	return e;
}

GF_Err gf_laser_decode_command_list(GF_LASeRCodec *codec, u16 ESID, char *data, u32 data_len, GF_List *com_list)
{
	GF_Err e;
	if (!codec || !data || !data_len) return GF_BAD_PARAM;

	codec->info = lsr_get_stream(codec, ESID);
	if (!codec->info) return GF_BAD_PARAM;
	codec->coord_bits = codec->info->cfg.coord_bits;
	codec->scale_bits = codec->info->cfg.scale_bits;
	codec->time_resolution = codec->info->cfg.time_resolution;
	codec->color_scale = (1<<codec->info->cfg.colorComponentBits) - 1;
	if (codec->info->cfg.resolution>=0)
		codec->res_factor = INT2FIX(1<<codec->info->cfg.resolution);
	else 
		codec->res_factor = gf_divfix(FIX_ONE, INT2FIX(1 << (-codec->info->cfg.resolution)) );

	codec->bs = gf_bs_new(data, data_len, GF_BITSTREAM_READ);
	e = lsr_decode_laser_unit(codec, com_list);
	gf_bs_del(codec->bs);
	codec->bs = NULL;
	return e;
}

void gf_laser_decoder_set_clock(GF_LASeRCodec *codec, Double (*GetSceneTime)(void *st_cbk), void *st_cbk )
{
	codec->GetSceneTime = GetSceneTime;
	codec->cbk = st_cbk;
}

static u32 lsr_read_vluimsbf5(GF_LASeRCodec *lsr, const char *name)
{
	u32 nb_words = 0;
	u32 nb_tot, nb_bits, val;

	while (gf_bs_read_int(lsr->bs, 1)) nb_words++;
	nb_words++;
	nb_tot = nb_words;
	nb_bits = nb_words*4;
	nb_tot += nb_bits;
	val = gf_bs_read_int(lsr->bs, nb_bits);
	lsr_dec_log_bits(lsr, val, nb_tot, name);
	return val;
}
static u32 lsr_read_vluimsbf8(GF_LASeRCodec *lsr, const char *name)
{
	u32 nb_words = 0;
	u32 nb_tot, nb_bits, val;

	while (gf_bs_read_int(lsr->bs, 1)) nb_words++;
	nb_words++;
	nb_tot = nb_words;
	nb_bits = nb_words*7;
	nb_tot += nb_bits;
	val = gf_bs_read_int(lsr->bs, nb_bits);
	lsr_dec_log_bits(lsr, val, nb_tot, name);
	return val;
}

static void lsr_read_private_element_container(GF_LASeRCodec *lsr)
{
	u32 val, len;
	GF_LSR_READ_INT(lsr, val, 2, "privateDataType");
	len = lsr_read_vluimsbf5(lsr, "len");
	gf_bs_align(lsr->bs);
#if 1
	gf_bs_skip_bytes(lsr->bs, len);
#else
	switch (val) {
	case 0: // private data of type "anyXML"
  		aligned privateChildren pc;
  		break;
  	case 1:
  		aligned uint(nameSpaceIndexBits) nameSpaceIndex;
    	aligned byte[len - ((nameSpaceIndexBits+7)>>3)] data;
    	break;
    default:
    	aligned byte[len] reserved;// ISO reserved
	}
#endif
}

static void lsr_read_private_attribute_container(GF_LASeRCodec *lsr)
{
	u32 val;
	do {
		u32 skip_len;
		GF_LSR_READ_INT(lsr, val, 2, "privateDataType");
		skip_len = lsr_read_vluimsbf5(lsr, "skipLen");
		gf_bs_align(lsr->bs);
		/*just skip data*/
#if 1
		gf_bs_skip_bytes(lsr->bs, skip_len);
#else 
		switch (val) {
		/*private data of type "anyXML"*/
		case 0:
			count = lsr_read_vluimsbf5(lsr, "count");
			for (i=0; i<count; i++) {
				privateAttribute(0) attr[i];
			}
			break;
		case 1:
			/*TODO FIXME - nameSpaceIndexBits is not defined in the spec*/
			uint(nameSpaceIndexBits) nameSpaceIndex;
			gf_bs_align(lsr->bs);
			byte[skipLen - ((nameSpaceIndexBits+7)%8)] data;
            break;
		default:
			/*TODO - spec is wrong here (typo, "len" instead of "skipLen" )*/
			gf_bs_skip_bytes(skipLen);
			break;
        }
#endif
		gf_bs_align(lsr->bs);
		GF_LSR_READ_INT(lsr, val, 1, "hasMorePrivateData");
    } while (val);
}

static void lsr_read_any_attribute(GF_LASeRCodec *lsr, GF_Node *node)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, "has_attrs");
	if (val) {
		do {
			GF_LSR_READ_INT(lsr, val, lsr->info->cfg.extensionIDBits, "reserved");
		    val = lsr_read_vluimsbf5(lsr, "len");//len in BITS
			GF_LSR_READ_INT(lsr, val, val, "reserved_val");
			GF_LSR_READ_INT(lsr, val, 1, "hasNextExtension");
		} while (val);
	}
}

static void lsr_read_object_content(GF_LASeRCodec *lsr, SVGElement *elt)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, "opt_group");
	if (val) lsr_read_private_attribute_container(lsr);
}

static void lsr_read_extension(GF_LASeRCodec *lsr, const char *name)
{
	u32 len = lsr_read_vluimsbf5(lsr, name);
#if 0
	*out_data = malloc(sizeof(char)*len);
	gf_bs_read_data(lsr->bs, *out_data, len);
	*out_len = len;
#else
	while (len) { gf_bs_read_int(lsr->bs, 8); len--; }
#endif
}

static void lsr_read_extend_class(GF_LASeRCodec *lsr, char **out_data, u32 *out_len, const char *name)
{
	u32 len;
	GF_LSR_READ_INT(lsr, len, lsr->info->cfg.extensionIDBits, "reserved");
	len = lsr_read_vluimsbf5(lsr, "byteLength");
	*out_data = malloc(sizeof(char)*len);
	gf_bs_read_data(lsr->bs, *out_data, len);
	*out_len = len;
}

static void lsr_read_codec_IDREF(GF_LASeRCodec *lsr, SVG_IRI *href, const char *name)
{
	GF_Node *n;
	u32 nID = 1+lsr_read_vluimsbf5(lsr, name);
	n = gf_sg_find_node(lsr->sg, nID);
	if (!n) {
		fprintf(stdout, "ERROR: undefined node\n");
		return;
	}
	href->target = (SVGElement *)n;
	href->type = SVG_IRI_ELEMENTID;
	gf_svg_register_iri(lsr->sg, href);
}
static void lsr_read_codec_IDREF_URI(GF_LASeRCodec *lsr, unsigned char **out_uri, const char *name)
{
	char szN[100];
	GF_Node *n;
	u32 nID = 1+lsr_read_vluimsbf5(lsr, name);
	n = gf_sg_find_node(lsr->sg, nID);
	if (!n) {
		fprintf(stdout, "ERROR: undefined node\n");
		return;
	}
	if (lsr->info->cfg.has_string_ids) {
		sprintf(szN, "#%s", gf_node_get_name(n));
	} else {
		sprintf(szN, "#%d", nID);
	}
	if (*out_uri) free(*out_uri);
	*out_uri = strdup(szN);
}

static void lsr_read_vl5string(GF_LASeRCodec *lsr, char **out_string, const char *name)
{
	u32 len = lsr_read_vluimsbf5(lsr, name);
	if (*out_string) free(*out_string);
	*out_string = malloc(sizeof(char)*(len+1));
	gf_bs_read_data(lsr->bs, *out_string, len);
	*out_string[len] = 0;
}

static Fixed lsr_read_fixed_16_8(GF_LASeRCodec *lsr, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 24, name);
	if (val & (1<<23)) {
		val &= ~(1<<23);
		return -INT2FIX(val) / 256;
	} else {
		return INT2FIX(val) / 256;
	}
}

static void lsr_read_fixed_16_8i(GF_LASeRCodec *lsr, SVG_Number *n, const char *name)
{
	s32 val;
	GF_LSR_READ_INT(lsr, val, 1, name);
	/*TODO FIXME - not clear with current impl of SVG number...*/
	if (val) {
		n->type=SVG_NUMBER_INHERIT;
	} else {
		n->type=SVG_NUMBER_VALUE;
		n->value = lsr_read_fixed_16_8(lsr, name);
	}
}


static const char *lsr_get_font(GF_LASeRCodec *lsr, u32 idx)
{
	return gf_list_get(lsr->font_table, idx);
}

static void lsr_get_color(GF_LASeRCodec *lsr, u32 idx, SVG_Color *color)
{
	LSRCol *c;
	if (idx>=lsr->nb_cols) return;

	c = &lsr->col_table[idx];
	color->red = INT2FIX(c->r) / lsr->color_scale;
	color->green = INT2FIX(c->g) / lsr->color_scale;
	color->blue = INT2FIX(c->b) / lsr->color_scale;
	color->type = SVG_COLOR_RGBCOLOR;
}

static void lsr_read_color(GF_LASeRCodec *lsr, SVG_Color *color, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, "hasIndex");
	if (val) {
		GF_LSR_READ_INT(lsr, val, lsr->colorIndexBits, name);
		lsr_get_color(lsr, val, color);
	} else {
		GF_LSR_READ_INT(lsr, val, 1, "isEnum");
		if (val) {
			GF_LSR_READ_INT(lsr, val, 2, "type");
			if (val==2) color->type = /*SVG_COLOR_NONE*/SVG_COLOR_INHERIT;
			else if (val==1) color->type = SVG_COLOR_CURRENTCOLOR;
			else color->type = SVG_COLOR_INHERIT;
		} else {
			lsr_read_extension(lsr, "colorExType0");
		}
	}
}


static void lsr_read_line_increment_type(GF_LASeRCodec *lsr, SVG_LineIncrement *li, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, "choice");
	if (val==1) {
		GF_LSR_READ_INT(lsr, val, 1, "type");
		if (val==1) li->type=SVG_NUMBER_INHERIT;
		else li->type=SVG_NUMBER_AUTO;
	} else {
		li->value = lsr_read_fixed_16_8(lsr, "line-increment-value");
	}
}

static void lsr_read_byte_align_string(GF_LASeRCodec *lsr, unsigned char **str, const char *name)
{
	u32 len;
	gf_bs_align(lsr->bs);
	len = lsr_read_vluimsbf8(lsr, "len");
	if (str) {
		if (*str) free(*str);
		*str = malloc(sizeof(char)*(len+1));
		gf_bs_read_data(lsr->bs, *str, len);
		(*str) [len] = 0;
	} else {
		while (len) { gf_bs_read_int(lsr->bs, 8); len--; }
	}
	lsr_dec_log_bits(lsr, 0, 8*len, name);
}
static void lsr_read_byte_align_string_list(GF_LASeRCodec *lsr, GF_List *l, const char *name)
{
	unsigned char *text, *sep, *sep2, *cur;
	while (gf_list_count(l)) {
		char *str = gf_list_last(l);
		gf_list_rem_last(l);
		free(str);
	}
	lsr_read_byte_align_string(lsr, &text, name);
	cur = text;
	while (1) {
		sep = strchr(cur, '\'');
		if (!sep) {
			gf_list_add(l, strdup(cur));
			break;
		}
		sep2 = strchr(sep + 1, '\'');
		if (!sep2) {
			gf_list_add(l, strdup(cur));
			break;
		}
		sep2[0] = 0;
		gf_list_add(l, strdup(sep+1));
		sep2[0] = '\'';
		cur = sep2 + 1;
	}
	free(text);
}

static void lsr_read_any_uri(GF_LASeRCodec *lsr, SVG_IRI *iri, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, "hasUri");
	if (val) {
		unsigned char *s;
		iri->type=SVG_IRI_IRI;
		lsr_read_byte_align_string(lsr, &s, "uri");
		GF_LSR_READ_INT(lsr, val, 1, "hasData");
		if (!val) {
			iri->iri = s;
		} else {
			unsigned char *data;
			u32 len;
			lsr_read_byte_align_string(lsr, &data, "uri");
			len = strlen(data) + 1 + strlen(s) + 1;
			iri->iri = malloc(sizeof(char) * len);
			strcpy(iri->iri, s);
			strcat(iri->iri, ",");
			strcat(iri->iri, data);
			iri->iri[len] = 0;
			free(s);
			free(data);
		}
    }
	GF_LSR_READ_INT(lsr, val, 1, "hasID");
	if (val) {
		iri->type=SVG_IRI_ELEMENTID;
		lsr_read_codec_IDREF(lsr, iri, "idref");
	}
	GF_LSR_READ_INT(lsr, val, 1, "hasStreamID");
	if (val) {
		u32 streamID = lsr_read_vluimsbf5(lsr, name);
	}
}

static void lsr_read_paint(GF_LASeRCodec *lsr, SVG_Paint *paint, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, "hasIndex");
	if (val) {
		GF_LSR_READ_INT(lsr, val, lsr->colorIndexBits, name);
		lsr_get_color(lsr, val, &paint->color);
		paint->type = SVG_PAINT_COLOR;
		paint->color.type = 0;
	} else {
		GF_LSR_READ_INT(lsr, val, 1, "isEnum");
		if (val) {
			GF_LSR_READ_INT(lsr, val, 2, "isEnum");
			switch (val) {
			case 0: paint->type = SVG_PAINT_INHERIT; break;
			case 1: paint->type = SVG_PAINT_COLOR; break;
			default: paint->type = SVG_PAINT_NONE; break;
			}
		} else {
			GF_LSR_READ_INT(lsr, val, 1, "isURI");
			if (val) {
				SVG_IRI iri;
				lsr_read_any_uri(lsr, &iri, name);
				paint->uri = iri.iri;
				paint->type = SVG_PAINT_URI;
			} else {
				lsr_read_extension(lsr, name);
			}
		}
	}
}

static void lsr_read_string_attribute(GF_LASeRCodec *lsr, unsigned char **class_attr, char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, name);
	if (val) lsr_read_byte_align_string(lsr, class_attr, name);
}
static void lsr_read_id(GF_LASeRCodec *lsr, GF_Node *n)
{
	u32 val, id;
	unsigned char *name;
	GF_LSR_READ_INT(lsr, val, 1, "has__0_id");
	if (!val) return;
	
	name = NULL;
    id = 1+lsr_read_vluimsbf5(lsr, "ID");
    if (lsr->info->cfg.has_string_ids) lsr_read_byte_align_string(lsr, &name, "stringId");
	gf_node_set_id(n, id, name);

	GF_LSR_READ_INT(lsr, val, 1, "reserved");
#if TODO_LASER_EXTENSIONS	
	if (val) {
		u32 len = lsr_read_vluimsbf5(lsr, "len");
		GF_LSR_READ_INT(lsr, val, len, "reserved");
	}
#endif
}

static Fixed lsr_translate_coords(GF_LASeRCodec *lsr, u32 val, u32 nb_bits)
{
	if (val >> (nb_bits-1) ) {
		s32 neg = (s32) val - (1<<nb_bits);
		return gf_divfix(INT2FIX(neg), lsr->res_factor);
	} else {
		return gf_divfix(INT2FIX(val), lsr->res_factor);
	}
}

static Fixed lsr_translate_scale(GF_LASeRCodec *lsr, u32 val)
{
	if (val >> (lsr->coord_bits-1) ) {
		s32 neg = (s32) val - (1<<lsr->coord_bits);
		return INT2FIX(neg) / 256;
	} else {
		return INT2FIX(val) / 256;
	}
}
static void lsr_read_matrix(GF_LASeRCodec *lsr, GF_Matrix2D *mx)
{
	u32 flag;
	gf_mx2d_init(*mx);
	GF_LSR_READ_INT(lsr, flag, 1, "isNotMatrix");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "isRef");
		if (flag) {
			GF_LSR_READ_INT(lsr, flag, 1, "hasXY");
			if (flag) {
				mx->m[2] = lsr_read_fixed_16_8(lsr, "valueX");
				mx->m[5] = lsr_read_fixed_16_8(lsr, "valueY");
			}
		} else {
			lsr_read_extension(lsr, "ext");
		}
	} else {
		lsr->coord_bits += lsr->scale_bits;
		GF_LSR_READ_INT(lsr, flag, 1, "xx_yy_present");
		if (flag) {
			GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "xx");
			mx->m[0] = lsr_translate_scale(lsr, flag);
			GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "yy");
			mx->m[4] = lsr_translate_scale(lsr, flag);
		}
		GF_LSR_READ_INT(lsr, flag, 1, "xy_yx_present");
		if (flag) {
			GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "xy");
			mx->m[1] = lsr_translate_scale(lsr, flag);
			GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "yx");
			mx->m[3] = lsr_translate_scale(lsr, flag);
		}
		lsr->coord_bits -= lsr->scale_bits;

		GF_LSR_READ_INT(lsr, flag, 1, "xz_yz_present");
		if (flag) {
			GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "xz");
			mx->m[2] = lsr_translate_coords(lsr, flag, lsr->coord_bits);
			GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "yz");
			mx->m[5] = lsr_translate_coords(lsr, flag, lsr->coord_bits);
		}
	}
}

static Fixed lsr_read_fixed_clamp(GF_LASeRCodec *lsr, const char *name)
{
	s32 val;
	GF_LSR_READ_INT(lsr, val, 8, name);
	return INT2FIX(val) / 255;
}

static void lsr_read_focus(GF_LASeRCodec *lsr, void *foc, const char *name)
{
	fprintf(stdout, "ERROR: FOCUS NOT IMPLEMENTED\n");
}

static void lsr_restore_base(GF_LASeRCodec *lsr, SVGElement *elt, SVGElement *base, Bool reset_fill, Bool reset_stroke)
{
	if (base->core->_class) elt->core->_class = strdup(base->core->_class);
	elt->core->eRR = base->core->eRR;
	if (base->properties) {
		memcpy(elt->properties, base->properties, sizeof(SVGProperties));
		if (base->properties->font_family.value) elt->properties->font_family.value = strdup(base->properties->font_family.value);
		if (base->properties->stroke_dasharray.array.vals) {
			u32 size = sizeof(Fixed)*base->properties->stroke_dasharray.array.count;
			elt->properties->stroke_dasharray.array.vals = malloc(size);
			memcpy(elt->properties->stroke_dasharray.array.vals, base->properties->stroke_dasharray.array.vals, size);
		}
		if (reset_fill) memset(&elt->properties->fill, 0, sizeof(SVG_Paint));
		if (reset_stroke) memset(&elt->properties->stroke, 0, sizeof(SVG_Paint));
	}
}

static void lsr_read_time_list(GF_LASeRCodec *lsr, GF_List *l, const char *name)
{
	SMIL_Time *v;
	u32 val, i, count;
	while (gf_list_count(l)) {
		v = gf_list_last(l);
		gf_list_rem_last(l);
		if (v->element_id) free(v->element_id);
		free(v);
	}
	GF_LSR_READ_INT(lsr, val, 1, name);
	if (!val) return;
	GF_LSR_READ_INT(lsr, val, 1, "choice");
	if (val) {
		GF_SAFEALLOC(v, sizeof(SMIL_Time));
		v->type = SMIL_TIME_INDEFINITE;
		gf_list_add(l, v);
		return;
	}
	count = lsr_read_vluimsbf5(lsr, "count");
	for (i=0; i<count; i++) {
		u32 now = lsr_read_vluimsbf5(lsr, "value");
		GF_SAFEALLOC(v, sizeof(SMIL_Time));
		v->type = SMIL_TIME_CLOCK;
		v->clock = now;
		v->clock /= lsr->time_resolution;
		gf_list_add(l, v);
	} 
}

static void lsr_read_duration(GF_LASeRCodec *lsr, SMIL_Duration *smil, const char *name)
{
	u32 val;
	smil->type = 0;
	GF_LSR_READ_INT(lsr, val, 1, "has_dur");
	if (!val) return;

	GF_LSR_READ_INT(lsr, val, 1, "choice");
	if (val) {
		GF_LSR_READ_INT(lsr, val, 1, "time");
		smil->type = val ? SMIL_DURATION_MEDIA : SMIL_DURATION_INDEFINITE;
	} else {
		Bool sign;
		u32 now;
		GF_LSR_READ_INT(lsr, sign, 1, "sign");
		now = lsr_read_vluimsbf5(lsr, "value");
		smil->clock_value = now;
		smil->clock_value /= lsr->time_resolution;
		if (sign) smil->clock_value *= -1; 
		smil->type = SMIL_DURATION_DEFINED;
	}
}

/*TODO Add decent error checking...*/
static void lsr_read_rare_full(GF_LASeRCodec *lsr, SVGElement *n, SVG_Matrix *matrix)
{
	u32 i, nb_rare, field_rare;
	GF_LSR_READ_INT(lsr, nb_rare, 1, "has__0_rare");
	if (!nb_rare) return;
	GF_LSR_READ_INT(lsr, nb_rare, 6, "nbOfAttributes");

	for (i=0; i<nb_rare; i++) {
		GF_LSR_READ_INT(lsr, field_rare, 6, "attributeRARE");
		switch (field_rare) {
		/*properties*/
		/*TODO !!!! what about inherit types??*/

		case RARE_AUDIO_LEVEL: n->properties->audio_level.value = lsr_read_fixed_clamp(lsr, "audio-level"); break;
	    case RARE_COLOR: lsr_read_paint(lsr, &n->properties->color, "color"); break;
		case RARE_COLOR_RENDERING: GF_LSR_READ_INT(lsr, n->properties->color_rendering, 2, "color-rendering"); break;
		case RARE_DISPLAY: GF_LSR_READ_INT(lsr, n->properties->display, 5, "display"); break;
	    case RARE_DISPLAY_ALIGN: GF_LSR_READ_INT(lsr, n->properties->display_align, 2, "display-align"); break;
		case RARE_FILL_OPACITY: n->properties->fill_opacity.value = lsr_read_fixed_clamp(lsr, "fill-opacity"); break;
	    case RARE_FILL_RULE: GF_LSR_READ_INT(lsr, n->properties->fill_rule, 2, "fill-rule"); break;
		case RARE_IMAGE_RENDERING: GF_LSR_READ_INT(lsr, n->properties->image_rendering, 2, "image-rendering"); break;
		case RARE_LINE_INCREMENT: lsr_read_line_increment_type(lsr, &n->properties->line_increment, "line-increment"); break;
	    case RARE_POINTER_EVENTS: GF_LSR_READ_INT(lsr, n->properties->pointer_events, 4, "pointer-events"); break;
		case RARE_SHAPE_RENDERING: GF_LSR_READ_INT(lsr, n->properties->shape_rendering, 3, "shape-rendering"); break;
	    case RARE_SOLID_COLOR: lsr_read_paint(lsr, &n->properties->solid_color, "solid-color"); break;
		case RARE_SOLID_OPACITY: n->properties->solid_opacity.value = lsr_read_fixed_clamp(lsr, "solid-opacity"); break;
	    case RARE_STOP_COLOR: lsr_read_paint(lsr, &n->properties->stop_color, "stop-color"); break;
		case RARE_STOP_OPACITY: n->properties->stop_opacity.value = lsr_read_fixed_clamp(lsr, "stop-opacity"); break;
		case RARE_STROKE_DASHARRAY:  
		{
			u32 j, flag;
			SVG_StrokeDashArray *da = &n->properties->stroke_dasharray;
			if (da->array.vals) free(da->array.vals);
			da->array.vals = NULL; da->array.count = 0; da->type = 0;
			GF_LSR_READ_INT(lsr, flag, 1, "dashArray");
			if (flag) {
				da->type=SVG_STROKEDASHARRAY_INHERIT;
			} else {
				da->type=SVG_STROKEDASHARRAY_ARRAY;
				da->array.count = lsr_read_vluimsbf5(lsr, "len");
				da->array.vals = malloc(sizeof(Fixed)*da->array.count);
		        for (j=0; j<da->array.count; j++) {
		            da->array.vals[j] = lsr_read_fixed_16_8(lsr, "dash");
				}
			}
		}
			break;
		case RARE_STROKE_DASHOFFSET: lsr_read_fixed_16_8i(lsr, &n->properties->stroke_dashoffset, "dashOffset"); break;

		/*TODO FIXME - SVG values are not in sync with LASeR*/
		case RARE_STROKE_LINECAP: GF_LSR_READ_INT(lsr, n->properties->stroke_linecap, 2, "stroke-linecap"); break;
		case RARE_STROKE_LINEJOIN: GF_LSR_READ_INT(lsr, n->properties->stroke_linejoin, 2, "stroke-linejoin"); break;
		case RARE_STROKE_MITERLIMIT: lsr_read_fixed_16_8i(lsr, &n->properties->stroke_miterlimit, "miterLimit"); break;
		case RARE_STROKE_OPACITY: n->properties->stroke_opacity.value = lsr_read_fixed_clamp(lsr, "stroke-opacity"); break;
		case RARE_STROKE_WIDTH: lsr_read_fixed_16_8i(lsr, &n->properties->stroke_width, "strokeWidth"); break;
		case RARE_TEXT_ANCHOR: GF_LSR_READ_INT(lsr, n->properties->text_anchor, 2, "text-achor"); break;
		case RARE_TEXT_RENDERING: GF_LSR_READ_INT(lsr, n->properties->text_rendering, 2, "text-rendering"); break;
	    case RARE_VIEWPORT_FILL: lsr_read_paint(lsr, &n->properties->viewport_fill, "viewport-fill"); break;
		case RARE_VIEWPORT_FILL_OPACITY: n->properties->viewport_fill_opacity.value = lsr_read_fixed_clamp(lsr, "viewport-fill-opacity"); break;
		case RARE_VECTOR_EFFECT: GF_LSR_READ_INT(lsr, n->properties->vector_effect, 4, "vector-effect"); break;
	    case RARE_VISIBILITY: GF_LSR_READ_INT(lsr, n->properties->visibility, 2, "visibility"); break;

		case RARE_FONT_FAMILY:
		{
			u32 flag;
			GF_LSR_READ_INT(lsr, flag, 1, "isInherit");
			if (n->properties->font_family.value) free(n->properties->font_family.value);			
			if (flag) {
				n->properties->font_family.type = SVG_FONTFAMILY_INHERIT;
			} else {
				char *ft;
				GF_LSR_READ_INT(lsr, flag, lsr->fontIndexBits, "fontIndex");
				ft = gf_list_get(lsr->font_table, flag);
				if (ft) n->properties->font_family.value = strdup(ft);
				n->properties->font_family.type = SVG_FONTFAMILY_VALUE;
			}
		}
			break;
		case RARE_FONT_SIZE: lsr_read_fixed_16_8i(lsr, &n->properties->font_size, "fontSize"); break;
		/*TODO not specified in spec !!*/
		case RARE_FONT_STYLE: GF_LSR_READ_INT(lsr, n->properties->font_style, 5, "fontStyle"); break;
		/*TODO not specified in spec !!*/
		case RARE_FONT_WEIGHT: GF_LSR_READ_INT(lsr, n->properties->font_weight, 4, "fontWeight"); break;

		case RARE_TRANSFORM: 
			if (!matrix) return;
			lsr_read_matrix(lsr, matrix);
			break;
#if 0
		case RARE_REQUIREDEXTENSIONS: lsr_read_byte_align_string_list(lsr, , "requiredExtensions");
	    case RARE_REQUIREDFORMATS: lsr_read_byte_align_string_list(lsr, *(GF_List **)fi->far_ptr, "requiredFormats");
	    case RARE_REQUIREDFEATURES: lsr_read_byte_align_string_list(lsr, *(GF_List **)fi->far_ptr, "requiredFeatures");
	    case RARE_SYSTEMLANGUAGE: lsr_read_byte_align_string_list(lsr, *(GF_List **)fi->far_ptr, "systemLanguage");
	    case RARE_XML_BASE: lsr_read_byte_align_string(lsr, *(SVG_String *)fi->far_ptr, "xml:base"); break;
	    case RARE_XML_LANG: lsr_read_byte_align_string(lsr, *(SVG_String *)fi->far_ptr, "xml:lang"); break;
	    case RARE_XML_SPACE: GF_LSR_READ_INT(lsr, *(XML_Space *)fi->far_ptr, 1, "xml:space"); break;
		/*focusable*/
		case RARE_FOCUSNEXT: lsr_read_focus(lsr, fi->far_ptr, "focusNext"); break;
		case RARE_FOCUSNORTH: lsr_read_focus(lsr, fi->far_ptr, "focusNorth"); break;
		case RARE_FOCUSNORTHEAST: lsr_read_focus(lsr, fi->far_ptr, "focusNorthEast"); break;
		case RARE_FOCUSNORTHWEST: lsr_read_focus(lsr, fi->far_ptr, "focusNorthWest"); break;
		case RARE_FOCUSPREV: lsr_read_focus(lsr, fi->far_ptr, "focusPrev"); break;
		case RARE_FOCUSSOUTH: lsr_read_focus(lsr, fi->far_ptr, "focusSouth"); break;
		case RARE_FOCUSSOUTHEAST: lsr_read_focus(lsr, fi->far_ptr, "focusSouthEast"); break;
		case RARE_FOCUSSOUTHWEST: lsr_read_focus(lsr, fi->far_ptr, "focusSouthWest"); break;
		case RARE_FOCUSWEST: lsr_read_focus(lsr, fi->far_ptr, "focusWest"); break;
		case RARE_FOCUSEAST: lsr_read_focus(lsr, fi->far_ptr, "focusEast"); break;
		case RARE_HREF_TITLE: 
		case RARE_HREF_TYPE: 
		case RARE_HREF_ROLE: 
		case RARE_HREF_ARCROLE: 
		case RARE_HREF_ACTUATE: 
		case RARE_HREF_SHOW: 
#endif
			break;
		case RARE_END: 
			lsr_read_time_list(lsr, n->timing->end, "end");
			break;
		case RARE_MIN: 
			lsr_read_duration(lsr, &n->timing->min, "min");
			break;
		case RARE_MAX: 
			lsr_read_duration(lsr, &n->timing->max, "min");
			break;
		}
	}
}

#define lsr_read_rare(_a, _b) lsr_read_rare_full(_a, _b, NULL)

static void lsr_read_fill(GF_LASeRCodec *lsr, SVGElement *n)
{
	Bool has_fill;
	GF_LSR_READ_INT(lsr, has_fill, 1, "has__1_fill");
	if (has_fill) lsr_read_paint(lsr, &n->properties->fill, "_1_fill");
}

static void lsr_read_stroke(GF_LASeRCodec *lsr, SVGElement *n)
{
	Bool has_stroke;
	GF_LSR_READ_INT(lsr, has_stroke, 1, "has__1_stroke");
	if (has_stroke) lsr_read_paint(lsr, &n->properties->stroke, "_1_stroke");
}
static void lsr_read_href(GF_LASeRCodec *lsr, SVGElement *elt)
{
	Bool has_href;
	GF_LSR_READ_INT(lsr, has_href, 1, "has_href");
	if (has_href) lsr_read_any_uri(lsr, &elt->xlink->href, "href");
}

static void lsr_read_accumulate(GF_LASeRCodec *lsr, u8 *accum_type)
{
	Bool v;
	GF_LSR_READ_INT(lsr, v, 1, "has__4_accumulate");
	if (v) {
		GF_LSR_READ_INT(lsr, *accum_type, 1, "_4_accumulate");
	}
	else *accum_type = 0;
}
static void lsr_read_additive(GF_LASeRCodec *lsr, u8 *add_type)
{
	Bool v;
	GF_LSR_READ_INT(lsr, v, 1, "has__4_additive");
	if (v) {
		GF_LSR_READ_INT(lsr, *add_type, 1, "_4_additive");
	}
	else *add_type = 0;
}
static void lsr_read_calc_mode(GF_LASeRCodec *lsr, u8 *calc_mode)
{
	u32 v;
	/*SMIL_CALCMODE_LINEAR is default and 0 in our code*/
	GF_LSR_READ_INT(lsr, v, 1, "has__4_calcMode");
	if (v) {
		GF_LSR_READ_INT(lsr, v, 2, "calcMode");
		/*SMIL_CALCMODE_DISCRETE is 0 in LASeR, 1 in our code*/
		if (!v) *calc_mode = 1;
		else *calc_mode = v;
	} else {
		*calc_mode = 0;
	}
}

static u32 lsr_read_animatable(GF_LASeRCodec *lsr, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 8, "attributeType");
	return val;
}

static void lsr_setup_attribute_name(GF_LASeRCodec *lsr, SVGElement *anim, SVGElement *anim_parent, SMIL_AttributeName *att_name)
{
	u32 i, count;
	GF_Node *n;
	s32 a_type = (s32) att_name->type;
	if (!anim->xlink->href.target) {
		anim->xlink->href.type = SVG_IRI_ELEMENTID;
		anim->xlink->href.target = anim_parent;
		gf_svg_register_iri(lsr->sg, &anim->xlink->href);
		n = (GF_Node *)anim_parent;
	} else {
		n = (GF_Node *)anim->xlink->href.target;
	}
	/*browse all anim types*/
	count = gf_node_get_field_count(n);
	for (i=0; i<count; i++) {
		GF_FieldInfo info;
		s32 the_type = gf_lsr_field_to_anim_type(n, i);
		if (the_type != a_type) continue;
		gf_node_get_field(n, i, &info);
		att_name->field_ptr = info.far_ptr;
		att_name->type = info.fieldType;
		/*and setup inited values*/
		if (anim->anim->from.value) anim->anim->from.type = info.fieldType;
		if (anim->anim->by.value) anim->anim->by.type = info.fieldType;
		if (anim->anim->to.value) anim->anim->to.type = info.fieldType;
		if (gf_list_count(anim->anim->values.values)) anim->anim->values.type = info.fieldType;
		att_name->name = strdup(info.name);
		return;
	}
}


static void lsr_read_single_time(GF_LASeRCodec *lsr, SMIL_Time *v, const char *name)
{
	u32 val;
	v->type = SMIL_TIME_INDEFINITE;
	GF_LSR_READ_INT(lsr, val, 1, name);
	if (!val) return;

	GF_LSR_READ_INT(lsr, val, 1, "choice");
	if (val) {
		GF_LSR_READ_INT(lsr, val, 1, "time");
		v->type = val ? SMIL_TIME_UNSPECIFIED : SMIL_TIME_INDEFINITE;
	} else {
		Bool sign;
		u32 now;
		GF_LSR_READ_INT(lsr, sign, 1, "sign");
		now = lsr_read_vluimsbf5(lsr, "value");
		v->type = SMIL_TIME_CLOCK;
		v->clock = now;
		v->clock /= lsr->time_resolution;
		if (sign) v->clock *= -1; 
	}
}


static void lsr_read_anim_fill(GF_LASeRCodec *lsr, u8 *animFreeze, const char *name)
{
	u32 val;

	GF_LSR_READ_INT(lsr, val, 1, name);
	if (val) {
		/*enumeration freeze{0} remove{1}*/
		GF_LSR_READ_INT(lsr, val, 1, name);
		*animFreeze = SMIL_FILL_REMOVE;
	} else {
		*animFreeze = SMIL_FILL_FREEZE;
	}
}
static void lsr_read_anim_repeat(GF_LASeRCodec *lsr, SMIL_RepeatCount *repeat, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, name);
	if (!val) {
		repeat->type = SMIL_REPEATCOUNT_UNSPECIFIED;
	} else {
		GF_LSR_READ_INT(lsr, val, 1, name);
		if (val) repeat->type = SMIL_REPEATCOUNT_INDEFINITE;
		else {
			repeat->type = SMIL_REPEATCOUNT_DEFINED;
			repeat->count = lsr_read_fixed_16_8(lsr, name);
		}
	}
}
static void lsr_read_repeat_duration(GF_LASeRCodec *lsr, SMIL_Duration *smil, const char *name)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, name);
	if (!flag) {
		smil->type = SMIL_DURATION_UNSPECIFIED;
	} else {
		GF_LSR_READ_INT(lsr, flag, 1, name);
		if (flag) {
			smil->type = SMIL_DURATION_INDEFINITE;
		} else {
			smil->clock_value = (Double) lsr_read_vluimsbf5(lsr, name);
			smil->clock_value /= lsr->time_resolution;
			smil->type = SMIL_DURATION_DEFINED;
		}
	}
}
static void lsr_read_anim_restart(GF_LASeRCodec *lsr, u8 *animRestart, const char *name)
{
	u32 val;
	GF_LSR_READ_INT(lsr, val, 1, name);
	if (!val) {
		*animRestart = SMIL_RESTART_ALWAYS;
	} else {
		/*enumeration always{0} never{1} whenNotActive{2}*/
		GF_LSR_READ_INT(lsr, *animRestart, 2, name);
	}
}

static void *lsr_read_an_anim_value(GF_LASeRCodec *lsr, u32 type, const char *name)
{
	u32 flag;
	u8 *enum_val;
	u32 *id_val;
	char *string;
	SVG_Number *num;
	SVG_IRI *iri;
	SVG_Point *pt;
	SVG_Paint *paint;
	/*TODO spec is wrong here*/
	GF_LSR_READ_INT(lsr, flag, 1, "escapeFlag");
    if (flag) GF_LSR_READ_INT(lsr, flag, 2, "escapeEnum");

    switch(type) {
    case 0: 
		string = NULL;
		lsr_read_byte_align_string(lsr, &string, name); 
		return string;
    case 1: 
		num = malloc(sizeof(SVG_Number));
		num->type = SVG_NUMBER_VALUE;
		num->value = lsr_read_fixed_16_8(lsr, name); 
		return num;
    case 12: 
		GF_SAFEALLOC(iri, sizeof(SVG_IRI));
		lsr_read_any_uri(lsr, iri, name); 
		return iri;
#if TODO_LASER_EXTENSIONS	
    case 2: lsr_read_path(lsr, val->value, name); break;
    case 3: lsr_read_point_sequence(lsr, val->value, name); break;
#endif
    case 4:
		num = malloc(sizeof(SVG_Number));
		num->type = SVG_NUMBER_VALUE;
		num->value = lsr_read_fixed_clamp(lsr, name); 
		return num;
    case 5: 
		GF_SAFEALLOC(paint, sizeof(SVG_Paint));
		lsr_read_paint(lsr, paint, name); 
		return paint;
    case 6: 
		enum_val = malloc(sizeof(u8));
		*enum_val = lsr_read_vluimsbf5(lsr, name); 
		return enum_val;
    case 10: 
		id_val = malloc(sizeof(u32));
		*id_val = lsr_read_vluimsbf5(lsr, name); 
		return id_val;
    case 11:
	{
		SVG_FontFamily *ft;
		u32 idx = lsr_read_vluimsbf5(lsr, name);
		ft = malloc(sizeof(SVG_FontFamily));
		ft->type = SVG_FONTFAMILY_VALUE;
		ft->value = gf_list_get(lsr->font_table, idx);
		if (ft->value) ft->value = strdup(ft->value);
	}
	break;
    /*TODO check this is correct*/
	case 7:
	{
		GF_List *l = gf_list_new();
		u32 i, count;
		count = lsr_read_vluimsbf5(lsr, "count");
        for (i=0; i<count; i++) {
			u8 *v = malloc(sizeof(u8));
			*v = lsr_read_vluimsbf5(lsr, "val");
			gf_list_add(l, v);
        }
		return l;
	}

    /*TODO check this is correct*/
    case 8: // floats
	{
		GF_List *l = gf_list_new();
		u32 i, count;
		count = lsr_read_vluimsbf5(lsr, "count");
        for (i=0; i<count; i++) {
			Fixed *v = malloc(sizeof(Fixed));
			*v = lsr_read_fixed_16_8(lsr, "val");
			gf_list_add(l, v);
        }
		return l;
	}

	/*point */
    case 9:
		pt = malloc(sizeof(SVG_Point));
		GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "valX");
		pt->x = lsr_translate_coords(lsr, flag, lsr->coord_bits);
		GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, "valY");
		pt->y = lsr_translate_coords(lsr, flag, lsr->coord_bits);
		return pt;
    default:
		lsr_read_extension(lsr, name);
        break;
    }
	return NULL;
}

static void lsr_read_anim_value(GF_LASeRCodec *lsr, SMIL_AnimateValue *anim, const char *name)
{
	u32 val, type;
	GF_LSR_READ_INT(lsr, val, 1, name);
	if (!val) {
		anim->type = 0;
	} else {
		GF_LSR_READ_INT(lsr, type, 4, "type");
		anim->value = lsr_read_an_anim_value(lsr, type, name);
	}
}

static void lsr_read_anim_values(GF_LASeRCodec *lsr, SMIL_AnimateValues *anims, const char *name)
{
	u32 flag, i, count = 0;
	u32 type;
	
	GF_LSR_READ_INT(lsr, flag, 1, name);
	if (!flag) return;

	while (gf_list_count(anims->values)) {
		SMIL_AnimateValue *att = gf_list_last(anims->values);
		gf_list_rem_last(anims->values);
		/*TODO THIS IS WRONG FIXME*/
		free(att);
	}

	GF_LSR_READ_INT(lsr, type, 4, "type");
	count = lsr_read_vluimsbf5(lsr, "count");
	for (i=0; i<count; i++) {
		void *att = lsr_read_an_anim_value(lsr, type, name);
		if (att) gf_list_add(anims->values, att);
	}
}

static void lsr_read_fraction_12(GF_LASeRCodec *lsr, GF_List *l, const char *name)
{
	u32 i, count, flag;
	GF_LSR_READ_INT(lsr, count, 1, name);
	if (!count) return;
	while (gf_list_count(l)) {
		Fixed *f = gf_list_last(l);
		gf_list_rem_last(l);
		free(f);

	}
	count = lsr_read_vluimsbf5(lsr, "name");
	for (i=0; i<count; i++) {
		Fixed *f;
		GF_SAFEALLOC(f, sizeof(Fixed));
		GF_LSR_READ_INT(lsr, flag, 1, "hasShort");
		if (flag) {
			GF_LSR_READ_INT(lsr, flag, 1, "isZero");
			if (flag) *f = 0;
			else *f = FIX_ONE;
		} else {
			u32 v;
			GF_LSR_READ_INT(lsr, v, 12, "val");
			*f = INT2FIX(v) / 4096/*(1<<12)*/;
		}
	}
}
static void lsr_read_float_list(GF_LASeRCodec *lsr, GF_List *l, const char *name)
{
	u32 i, count = gf_list_count(l);
	GF_LSR_READ_INT(lsr, count, 1, name);
	if (!count) return;
	while (gf_list_count(l)) {
		Fixed *v = gf_list_last(l);
		gf_list_rem_last(l);
		free(v);
	}
	count = lsr_read_vluimsbf5(lsr, "count");
	for (i=0;i<count;i++) {
		Fixed *v = malloc(sizeof(Fixed));
		*v = lsr_read_fixed_16_8(lsr, "val");
		gf_list_add(l, v);
	}
}

static void lsr_read_point_sequence(GF_LASeRCodec *lsr, GF_List *pts, const char *name)
{
	u32 flag, i, count;
	
	while (gf_list_count(pts)) {
		SVG_Point *v = gf_list_last(pts);
		gf_list_rem_last(pts);
		free(v);
	}
	count = lsr_read_vluimsbf5(lsr, "nbPoints");
	if (!count) return;
	/*TODO golomb coding*/
	GF_LSR_READ_INT(lsr, flag, 1, "flag");
    if (!flag) {
        if (count < 3) {
			u32 nb_bits, v;
			GF_LSR_READ_INT(lsr, nb_bits, 5, "bits");
            for (i=0; i<count; i++) {
				SVG_Point *pt = malloc(sizeof(SVG_Point));
				gf_list_add(pts, pt);
				GF_LSR_READ_INT(lsr, v, nb_bits, "x");
				pt->x = lsr_translate_coords(lsr, v, nb_bits);
				GF_LSR_READ_INT(lsr, v, nb_bits, "y");
				pt->y = lsr_translate_coords(lsr, v, nb_bits);
            }
        } else {
			u32 nb_dx, nb_dy, k;
			Fixed x, y;
			SVG_Point *pt = malloc(sizeof(SVG_Point));
			gf_list_add(pts, pt);
			
			GF_LSR_READ_INT(lsr, nb_dx, 5, "bits");
			GF_LSR_READ_INT(lsr, k, nb_dx, "x");
			x = pt->x = lsr_translate_coords(lsr, k, nb_dx);
			GF_LSR_READ_INT(lsr, k, nb_dx, "y");
			y = pt->y = lsr_translate_coords(lsr, k, nb_dx);

			GF_LSR_READ_INT(lsr, nb_dx, 5, "bitsx");
			GF_LSR_READ_INT(lsr, nb_dy, 5, "bitsy");
			for (i=1; i<count; i++) {
				SVG_Point *pt = malloc(sizeof(SVG_Point));
				gf_list_add(pts, pt);
				GF_LSR_READ_INT(lsr, k, nb_dx, "dx"); 
				pt->x = x + lsr_translate_coords(lsr, k, nb_dx);
				x = pt->x;
				GF_LSR_READ_INT(lsr, k, nb_dy, "dy"); 
				pt->y = y + lsr_translate_coords(lsr, k, nb_dy);
				y = pt->y;
			}
        }
	}
}
static void lsr_read_path_type(GF_LASeRCodec *lsr, SVG_PathData *path, const char *name)
{
	u32 i, count;
	lsr_read_point_sequence(lsr, path->points, "seq");
	while (gf_list_count(path->commands)) {
		u8 *v = gf_list_last(path->commands);
		gf_list_rem_last(path->commands);
		free(v);
	}
    count = lsr_read_vluimsbf5(lsr, "nbOfTypes");
    for (i=0; i<count; i++) {
        u8 *type = malloc(sizeof(u8));
		gf_list_add(path->commands, type);
		GF_LSR_READ_INT(lsr, *type, 8, name);
    }
}

static void lsr_read_rotate_type(GF_LASeRCodec *lsr, SVG_Number *rotate, const char *name)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, name);
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "choice");
		if (flag) rotate->type = flag ? SVG_NUMBER_AUTO_REVERSE : SVG_NUMBER_AUTO;
	} else {
		rotate->value = lsr_read_fixed_16_8(lsr, "rotate");
		rotate->value = SVG_NUMBER_VALUE;
	}
}
static void lsr_read_sync_behavior(GF_LASeRCodec *lsr, u8 *sync, const char *name)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, name);
	if (!flag) *sync = SMIL_SYNCBEHAVIOR_INHERIT;
	else {
		GF_LSR_READ_INT(lsr, flag, 2, name);
		*sync = flag + 1;
	}
}
static void lsr_read_sync_tolerance(GF_LASeRCodec *lsr, SMIL_SyncTolerance *sync, const char *name)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, name);
	if (!flag) sync->type = SMIL_SYNCTOLERANCE_INHERIT;
	else {
		GF_LSR_READ_INT(lsr, flag, 1, name);
		if (flag) sync->type = SMIL_SYNCTOLERANCE_DEFAULT;
		else {
			u32 v = lsr_read_vluimsbf5(lsr, "value");
			sync->value = INT2FIX(v);
			sync->value /= lsr->time_resolution;
		}
	}
}
static void lsr_read_coordinate(GF_LASeRCodec *lsr, SVG_Number *coord, Bool skipable, const char *name)
{
	u32 flag;
	if (skipable) {
		GF_LSR_READ_INT(lsr, flag, 1, name);
		if (!flag) {
			coord->type = SVG_NUMBER_UNKNOWN;
			coord->value = 0;
			return;
		}
	} 
	coord->type = SVG_NUMBER_VALUE;
	GF_LSR_READ_INT(lsr, flag, lsr->coord_bits, name);
	coord->value = lsr_translate_coords(lsr, flag, lsr->coord_bits);
}
static void lsr_read_coord_list(GF_LASeRCodec *lsr, GF_List *coords, const char *name)
{
	u32 i, count;
	while (gf_list_count(coords)) {
		SVG_Coordinate *f = gf_list_last(coords);
		gf_list_rem_last(coords);
		free(f);
	}
	GF_LSR_READ_INT(lsr, count, 1, name);
	if (!count) return;
    count = lsr_read_vluimsbf5(lsr, "nb_coords");
	for (i=0; i<count; i++) {
		u32 res;
		SVG_Coordinate *f = malloc(sizeof(SVG_Coordinate ));
		GF_LSR_READ_INT(lsr, res, lsr->coord_bits, name);
		f->value = lsr_translate_coords(lsr, res, lsr->coord_bits);
		gf_list_add(coords, f);
	}
}

static void lsr_read_transform_behavior(GF_LASeRCodec *lsr, u8 *tr_type, const char *name)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, name);
	if (!flag) {
		*tr_type = 0;
	} else {
		GF_LSR_READ_INT(lsr, flag, 4, name);
		assert(0);
	}
}

static void lsr_read_content_type(GF_LASeRCodec *lsr, unsigned char **type, const char *name)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, "hasType");
	if (flag) lsr_read_byte_align_string(lsr, type, "type");
}
static void lsr_read_value_with_units(GF_LASeRCodec *lsr, SVG_Number *n, const char *name)
{
	s32 val;
	GF_LSR_READ_INT(lsr, val, 32, name);
#ifdef GPAC_FIXED_POINT
	n->value = val << 8;
#else
	n->value = INT2FIX(val) / (1<<8);
#endif
	GF_LSR_READ_INT(lsr, val, 3, "units");
    switch (val) {
	case 1: n->type = SVG_NUMBER_IN; break;
	case 2: n->type = SVG_NUMBER_CM; break;
	case 3: n->type = SVG_NUMBER_MM; break;
	case 4: n->type = SVG_NUMBER_PT; break;
	case 5: n->type = SVG_NUMBER_PC; break;
	case 6: n->type = SVG_NUMBER_PERCENTAGE; break;
	default:  n->type = SVG_NUMBER_VALUE; break;
	}
}

static void lsr_read_event_type(GF_LASeRCodec *lsr, u32 *evtType, unsigned char **evtName)
{
	u32 flag;
	GF_LSR_READ_INT(lsr, flag, 1, "choice");
	if (!flag) {
		lsr_read_byte_align_string(lsr, evtName, "evtString");
	} else {
		GF_LSR_READ_INT(lsr, flag, 6, "event");
		/*enumeration abort{0} activate{1} begin{2} click{3} end{4} error{5} focusin{6} focusout{7} 
		keydown{8} keypress{9} keyup{10} load{11} longkeypress{12} mousedown{13} mouseout{14} 
		mouseover{15} mouseup{16} repeat{17} resize{18} scroll{19} textinput{20} unload{21} zoom{22} */
		switch (flag) {
		case 0: *evtType = SVG_DOM_EVT_ABORT; break;
		case 1: *evtType = SVG_DOM_EVT_ACTIVATE; break;
		case 2: *evtType = SVG_DOM_EVT_BEGIN; break;
		case 3: *evtType = SVG_DOM_EVT_CLICK; break;
		case 4: *evtType = SVG_DOM_EVT_END; break;
		case 5: *evtType = SVG_DOM_EVT_ERROR; break;
		case 6: *evtType = SVG_DOM_EVT_FOCUSIN; break;
		case 7: *evtType = SVG_DOM_EVT_FOCUSOUT; break;
		case 8: *evtType = SVG_DOM_EVT_KEYDOWN; break;
		case 9: *evtType = SVG_DOM_EVT_KEYPRESS; break;
		case 10: *evtType = SVG_DOM_EVT_KEYUP; break;
		case 11: *evtType = SVG_DOM_EVT_LOAD; break;
		case 12: *evtType = SVG_DOM_EVT_LONGKEYPRESS; break;
		case 13: *evtType = SVG_DOM_EVT_MOUSEDOWN; break;
		case 14: *evtType = SVG_DOM_EVT_MOUSEOUT; break;
		case 15: *evtType = SVG_DOM_EVT_MOUSEOVER; break;
		case 16: *evtType = SVG_DOM_EVT_MOUSEUP; break;
		case 17: *evtType = SVG_DOM_EVT_REPEAT; break;
		case 18: *evtType = SVG_DOM_EVT_RESIZE; break;
		case 19: *evtType = SVG_DOM_EVT_SCROLL; break;
		case 20: *evtType = SVG_DOM_EVT_TEXTINPUT; break;
		case 21: *evtType = SVG_DOM_EVT_UNLOAD; break;
		case 22: *evtType = SVG_DOM_EVT_ZOOM; break;
		default:
			fprintf(stdout, "Unsupported LASER event\n");
			break;
		}
	}
}


static GF_Node *lsr_read_a(GF_LASeRCodec *lsr)
{
	Bool flag;
	SVGaElement *a = (SVGaElement *) gf_node_new(lsr->sg, TAG_SVG_a);
	lsr_read_string_attribute(lsr, &a->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) a);
	lsr_read_rare_full(lsr, (SVGElement *) a, &a->transform);
	lsr_read_fill(lsr, (SVGElement *) a);
	lsr_read_stroke(lsr, (SVGElement *) a);
	GF_LSR_READ_INT(lsr, a->core->eRR, 1, "externalResourcesRequired");
	GF_LSR_READ_INT(lsr, flag, 1, "hasTarget");
	if (flag) lsr_read_byte_align_string(lsr, &a->target, "target");
	lsr_read_href(lsr, (SVGElement *)a);
	lsr_read_any_attribute(lsr, (GF_Node *) a);
	lsr_read_group_content(lsr, (SVGElement *) a, 0);
	return (GF_Node *)a;
}

static GF_Node *lsr_read_animate(GF_LASeRCodec *lsr, SVGElement *parent)
{
	Bool flag;
	SVGanimateElement *elt = (SVGanimateElement *) gf_node_new(lsr->sg, TAG_SVG_animate);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	lsr_read_accumulate(lsr, &elt->anim->accumulate);
	lsr_read_additive(lsr, &elt->anim->additive);
	lsr_read_anim_value(lsr, &elt->anim->by, "_4_by");
	lsr_read_calc_mode(lsr, &elt->anim->calcMode);
	lsr_read_anim_value(lsr, &elt->anim->from, "_4_from");
	lsr_read_fraction_12(lsr, elt->anim->keySplines, "_4_keySplines");
	lsr_read_fraction_12(lsr, elt->anim->keyTimes, "_4_keyTimes");
	lsr_read_anim_values(lsr, &elt->anim->values, "_4_values");
	lsr_read_time_list(lsr, elt->timing->begin, "_5_begin");
	lsr_read_duration(lsr, &elt->timing->dur, "_5_dur");
	/*TODO lsr:enabled not present in our code*/
	GF_LSR_READ_INT(lsr, flag/*animate->enabled*/, 1, "enabled");
	lsr_read_anim_fill(lsr, &elt->timing->fill, "_5_fill");
	lsr_read_anim_repeat(lsr, &elt->timing->repeatCount, "repeatCount");
	lsr_read_repeat_duration(lsr, &elt->timing->repeatDur, "_5_repeatDur");
	lsr_read_anim_restart(lsr, &elt->timing->restart, "_5_restart");
	lsr_read_anim_value(lsr, &elt->anim->to, "_5_to");
	elt->anim->attributeName.type = lsr_read_animatable(lsr, "_5_attributeName");
	lsr_read_href(lsr, (SVGElement *) elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);

	lsr_setup_attribute_name(lsr, (SVGElement *)elt, parent, &elt->anim->attributeName);

	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}


static GF_Node *lsr_read_animateMotion(GF_LASeRCodec *lsr, SVGElement *parent)
{
	Bool flag;
	SVGanimateMotionElement *elt = (SVGanimateMotionElement *) gf_node_new(lsr->sg, TAG_SVG_animateMotion);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_accumulate(lsr, &elt->anim->accumulate);
	lsr_read_additive(lsr, &elt->anim->additive);
	lsr_read_anim_value(lsr, &elt->anim->by, "_4_by");
	lsr_read_calc_mode(lsr, &elt->anim->calcMode);
	lsr_read_anim_value(lsr, &elt->anim->from, "_4_from");
	lsr_read_fraction_12(lsr, elt->anim->keySplines, "_4_keySplines");
	lsr_read_fraction_12(lsr, elt->anim->keyTimes, "_4_keyTimes");
	lsr_read_anim_values(lsr, &elt->anim->values, "_4_values");
	lsr_read_time_list(lsr, elt->timing->begin, "_5_begin");
	lsr_read_duration(lsr, &elt->timing->dur, "_5_dur");
	/*TODO lsr:enabled not present in our code*/
	GF_LSR_READ_INT(lsr, flag/*elt->enabled*/, 1, "enabled");
	lsr_read_anim_fill(lsr, &elt->timing->fill, "_5_fill");
	lsr_read_anim_repeat(lsr, &elt->timing->repeatCount, "_5_repeatCount");
	lsr_read_repeat_duration(lsr, &elt->timing->repeatDur, "_5_repeatDur");
	lsr_read_anim_restart(lsr, &elt->timing->restart, "_5_restart");
	lsr_read_anim_value(lsr, &elt->anim->to, "_5_to");
	lsr_read_float_list(lsr, elt->keyPoints, "keyPoints");
	GF_LSR_READ_INT(lsr, flag, 1, "hasPath");
	if (flag)
		lsr_read_path_type(lsr, &elt->path, "path");
	lsr_read_rotate_type(lsr, &elt->rotate, "rotate");
	lsr_read_href(lsr, (SVGElement *)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	if (!elt->xlink->href.target) {
		elt->xlink->href.type = SVG_IRI_ELEMENTID;
		elt->xlink->href.target = parent;
		gf_svg_register_iri(lsr->sg, &elt->xlink->href);
	}
	/*and setup inited values*/
	if (elt->anim->from.value) elt->anim->from.type = SVG_Motion_datatype;
	if (elt->anim->by.value) elt->anim->by.type = SVG_Motion_datatype;
	if (elt->anim->to.value) elt->anim->to.type = SVG_Motion_datatype;
	if (gf_list_count(elt->anim->values.values)) elt->anim->values.type = SVG_Motion_datatype;

	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}

static GF_Node *lsr_read_animateTransform(GF_LASeRCodec *lsr, SVGElement *parent)
{
	Bool flag;
	SVGanimateTransformElement *elt= (SVGanimateTransformElement *) gf_node_new(lsr->sg, TAG_SVG_animateTransform);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	lsr_read_accumulate(lsr, &elt->anim->accumulate);
	lsr_read_additive(lsr, &elt->anim->additive);
	lsr_read_anim_value(lsr, &elt->anim->by, "_4_by");
	lsr_read_calc_mode(lsr, &elt->anim->calcMode);
	lsr_read_anim_value(lsr, &elt->anim->from, "_4_from");
	lsr_read_fraction_12(lsr, elt->anim->keySplines, "_4_keySplines");
	lsr_read_fraction_12(lsr, elt->anim->keyTimes, "_4_keyTimes");
	lsr_read_anim_values(lsr, &elt->anim->values, "_4_values");
	lsr_read_time_list(lsr, elt->timing->begin, "_5_begin");
	lsr_read_duration(lsr, &elt->timing->dur, "_5_dur");
	/*TODO lsr:enabled not present in our code*/
	GF_LSR_READ_INT(lsr, flag/*elt->enabled*/, 1, "enabled");
	lsr_read_anim_fill(lsr, &elt->timing->fill, "_5_fill");
	lsr_read_anim_repeat(lsr, &elt->timing->repeatCount, "_5_repeatCount");
	lsr_read_repeat_duration(lsr, &elt->timing->repeatDur, "_5_repeatDur");
	lsr_read_anim_restart(lsr, &elt->timing->restart, "_5_restart");
	lsr_read_anim_value(lsr, &elt->anim->to, "_5_to");
	elt->anim->attributeName.type = lsr_read_animatable(lsr, "_5_attributeName");

	/*enumeration rotate{0} scale{1} skewX{2} skewY{3} translate{4}*/
	GF_LSR_READ_INT(lsr, flag, 3, "rotscatra");
	switch (flag) {
	case 0: elt->anim->type = SVG_TRANSFORM_ROTATE; break;
	case 1: elt->anim->type = SVG_TRANSFORM_SCALE; break;
	case 2: elt->anim->type = SVG_TRANSFORM_SKEWX; break;
	case 3: elt->anim->type = SVG_TRANSFORM_SKEWY; break;
	case 4: elt->anim->type = SVG_TRANSFORM_TRANSLATE; break;
	}

	lsr_read_href(lsr, (SVGElement *)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);

	lsr_setup_attribute_name(lsr, (SVGElement *)elt, parent, &elt->anim->attributeName);
	
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}

static GF_Node *lsr_read_audio(GF_LASeRCodec *lsr, SVGElement *parent)
{
	Bool flag;
	SVGaudioElement *elt= (SVGaudioElement *) gf_node_new(lsr->sg, TAG_SVG_audio);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	lsr_read_time_list(lsr, elt->timing->begin, "_5_begin");
	lsr_read_duration(lsr, &elt->timing->dur, "_5_dur");
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_anim_repeat(lsr, &elt->timing->repeatCount, "_5_repeatCount");
	lsr_read_repeat_duration(lsr, &elt->timing->repeatDur, "_5_repeatDur");
	lsr_read_sync_behavior(lsr, &elt->sync->syncBehavior, "syncBehavior");
	lsr_read_sync_tolerance(lsr, &elt->sync->syncTolerance, "syncBehavior");
	lsr_read_content_type(lsr, &elt->xlink->type, "type");
	lsr_read_href(lsr, (SVGElement *)elt);

//	lsr_read_time(lsr, elt->clipBegin, "clipBegin");
	GF_LSR_READ_INT(lsr, flag, 1, "clipBegin");
//	lsr_read_time(lsr, elt->clipEnd, "clipEnd");
	GF_LSR_READ_INT(lsr, flag, 1, "clipEnd");
//	lsr_read_href(lsr, & elt->syncReference);
	GF_LSR_READ_INT(lsr, flag, 1, "hasSyncReference");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}
static GF_Node *lsr_read_circle(GF_LASeRCodec *lsr)
{
	SVGcircleElement *elt= (SVGcircleElement *) gf_node_new(lsr->sg, TAG_SVG_circle);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
	lsr_read_fill(lsr, (SVGElement*)elt);
	lsr_read_stroke(lsr, (SVGElement*)elt);
	lsr_read_coordinate(lsr, &elt->cx, 1, "cx");
	lsr_read_coordinate(lsr, &elt->cy, 1, "cy");
	lsr_read_coordinate(lsr, &elt->r, 0, "r");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}
static GF_Node *lsr_read_cursor(GF_LASeRCodec *lsr)
{
	/*TODO FIX CODE GENERATION*/
	SVGimageElement *elt = (SVGimageElement*) gf_node_new(lsr->sg, TAG_SVG_image);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	lsr_read_coordinate(lsr, &elt->x, 1, "x");
	lsr_read_coordinate(lsr, &elt->y, 1, "y");
	lsr_read_href(lsr, (SVGElement *)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}

static GF_Node *lsr_read_data(GF_LASeRCodec *lsr, u32 node_tag)
{
	SVGdescElement *elt = (SVGdescElement*) gf_node_new(lsr->sg, node_tag);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}

static GF_Node *lsr_read_defs(GF_LASeRCodec *lsr)
{
	SVGdefsElement *elt = (SVGdefsElement*) gf_node_new(lsr->sg, TAG_SVG_defs);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_fill(lsr, (SVGElement*)elt);
	lsr_read_stroke(lsr, (SVGElement*)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}
static GF_Node *lsr_read_ellipse(GF_LASeRCodec *lsr)
{
	SVGellipseElement *elt = (SVGellipseElement*) gf_node_new(lsr->sg, TAG_SVG_ellipse);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
	lsr_read_fill(lsr, (SVGElement*)elt);
	lsr_read_stroke(lsr, (SVGElement*)elt);
	lsr_read_coordinate(lsr, &elt->cx, 1, "cx");
	lsr_read_coordinate(lsr, &elt->cy, 1, "cy");
	lsr_read_coordinate(lsr, &elt->rx, 0, "rx");
	lsr_read_coordinate(lsr, &elt->ry, 0, "ry");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *) elt;
}
static GF_Node *lsr_read_foreignObject(GF_LASeRCodec *lsr)
{
	u32 flag;
	SVGforeignObjectElement *elt = (SVGforeignObjectElement *) gf_node_new(lsr->sg, TAG_SVG_foreignObject);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
	lsr_read_fill(lsr, (SVGElement*)elt);
	lsr_read_stroke(lsr, (SVGElement*)elt);
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_coordinate(lsr, &elt->height, 0, "height");
	lsr_read_coordinate(lsr, &elt->width, 0, "width");
	lsr_read_coordinate(lsr, &elt->x, 1, "x");
	lsr_read_coordinate(lsr, &elt->y, 1, "y");

	lsr_read_any_attribute(lsr, (GF_Node *) elt);
/*	TODO
	bit(1) opt_group;
	if(opt_group) {
		vluimsbf5 occ1;
		for(int t=0;t<occ1;t++) {
			privateElementContainer child0[[t]];
		}
	}
*/
	GF_LSR_READ_INT(lsr, flag, 1, "opt_group");
	return (GF_Node *) elt;
}
static GF_Node *lsr_read_g(GF_LASeRCodec *lsr, Bool is_same)
{
	u32 flag;
	SVGgElement *elt = (SVGgElement*) gf_node_new(lsr->sg, TAG_SVG_g);
	if (is_same) {
		if (lsr->prev_g) {
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_g, 0, 0);
			gf_mx2d_copy(elt->transform, lsr->prev_g->transform);
			/*TODO restore choice and size in LASeR*/
		}
		lsr_read_id(lsr, (GF_Node *) elt);
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement*)elt);
		lsr_read_stroke(lsr, (SVGElement*)elt);
		/*TODO choice only ibn LASeR, not in SVG*/
		GF_LSR_READ_INT(lsr, flag, 1, "choice");
		GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
		/*TODO size only in LASeR, not in SVG*/
		GF_LSR_READ_INT(lsr, flag, 1, "size");
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_g = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, is_same);
	return (GF_Node *) elt;
}
static GF_Node *lsr_read_image(GF_LASeRCodec *lsr)
{
	u32 flag;
	u8 fixme;
	SVGimageElement *elt = (SVGimageElement*) gf_node_new(lsr->sg, TAG_SVG_image);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_coordinate(lsr, &elt->height, 1, "height");
	GF_LSR_READ_INT(lsr, flag, 1, "opacity");
	if (flag) {
		elt->properties->opacity.type = SVG_NUMBER_VALUE;
		elt->properties->opacity.value = lsr_read_fixed_clamp(lsr, "opacity");
	}
	lsr_read_transform_behavior(lsr, &fixme, "transformBehavior");
	lsr_read_content_type(lsr, &elt->xlink->type, "type");
	lsr_read_coordinate(lsr, &elt->width, 1, "width");
	lsr_read_coordinate(lsr, &elt->x, 1, "x");
	lsr_read_coordinate(lsr, &elt->y, 1, "y");
	lsr_read_href(lsr, (SVGElement *)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_line(GF_LASeRCodec *lsr, Bool is_same)
{
	SVGlineElement *elt = (SVGlineElement*) gf_node_new(lsr->sg, TAG_SVG_line);

	if (is_same) {
		if (lsr->prev_line) {
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_line, 0, 0);
			gf_mx2d_copy(elt->transform, lsr->prev_line->transform);
		}
		lsr_read_id(lsr, (GF_Node *) elt);
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_stroke(lsr, (SVGElement *) elt);
	}
	lsr_read_coordinate(lsr, &elt->x1, 1, "x1");
	lsr_read_coordinate(lsr, &elt->x2, 0, "x2");
	lsr_read_coordinate(lsr, &elt->y1, 1, "y1");
	lsr_read_coordinate(lsr, &elt->y2, 0, "y2");
	if (!is_same) {
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_line = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, is_same);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_linearGradient(GF_LASeRCodec *lsr)
{
	u32 flag;
	SVGlinearGradientElement*elt = (SVGlinearGradientElement*) gf_node_new(lsr->sg, TAG_SVG_linearGradient);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_fill(lsr, (SVGElement *) elt);
	lsr_read_stroke(lsr, (SVGElement *) elt);
	GF_LSR_READ_INT(lsr, flag, 1, "hasGradientUnits");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "hasGradientUnits");
		/*enumeration objectBoundingBox{0} userSpaceOnUse{1}*/
		if (flag) elt->gradientUnits = SVG_GRADIENTUNITS_USER;
	}
	lsr_read_coordinate(lsr, &elt->x1, 1, "x1");
	lsr_read_coordinate(lsr, &elt->x2, 1, "x2");
	lsr_read_coordinate(lsr, &elt->y1, 1, "y1");
	lsr_read_coordinate(lsr, &elt->y2, 1, "y2");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_mpath(GF_LASeRCodec *lsr)
{
	SVGmpathElement *elt = (SVGmpathElement*) gf_node_new(lsr->sg, TAG_SVG_mpath);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	lsr_read_href(lsr, (SVGElement *)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_path(GF_LASeRCodec *lsr, u32 same_type)
{
	u32 flag;
	SVGpathElement *elt = (SVGpathElement*) gf_node_new(lsr->sg, TAG_SVG_path);

	if (same_type) {
		if (lsr->prev_path) {
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_path, (same_type==2) ? 1 : 0, 0);
			gf_mx2d_copy(elt->transform, lsr->prev_path->transform);
		}
		lsr_read_id(lsr, (GF_Node *) elt);
		if (same_type==2) lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_path_type(lsr, &elt->d, "d");
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_stroke(lsr, (SVGElement *) elt);
		lsr_read_path_type(lsr, &elt->d, "d");
		GF_LSR_READ_INT(lsr, flag, 1, "hasPathLength");
		if (flag) elt->pathLength.value = lsr_read_fixed_16_8(lsr, "pathLength");
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_path = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, same_type);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_polygon(GF_LASeRCodec *lsr, Bool is_polyline, u32 same_type)
{
	SVGpolygonElement *elt = (SVGpolygonElement*) gf_node_new(lsr->sg, is_polyline ? TAG_SVG_polyline : TAG_SVG_polygon);

	if (same_type) {
		if (lsr->prev_polygon) {
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_polygon, (same_type==2) ? 1 : 0, (same_type==3) ? 1 : 0);
			gf_mx2d_copy(elt->transform, lsr->prev_polygon->transform);
		}
		lsr_read_id(lsr, (GF_Node *) elt);
		if (same_type==2) lsr_read_fill(lsr, (SVGElement *) elt);
		else if (same_type==3) lsr_read_stroke(lsr, (SVGElement *) elt);
		lsr_read_point_sequence(lsr, elt->points, "points");
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_stroke(lsr, (SVGElement *) elt);
		lsr_read_point_sequence(lsr, elt->points, "points");
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_polygon = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, same_type);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_radialGradient(GF_LASeRCodec *lsr)
{
	u32 flag;
	SVGradialGradientElement *elt = (SVGradialGradientElement*) gf_node_new(lsr->sg, TAG_SVG_radialGradient);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_fill(lsr, (SVGElement *) elt);
	lsr_read_stroke(lsr, (SVGElement *) elt);
	lsr_read_coordinate(lsr, &elt->cx, 1, "cx");
	lsr_read_coordinate(lsr, &elt->cy, 1, "cy");

	/*enumeration objectBoundingBox{0} userSpaceOnUse{1}*/
	GF_LSR_READ_INT(lsr, flag, 1, "hasGradientUnits");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "radientUnits");
		if (flag) elt->gradientUnits = SVG_GRADIENTUNITS_USER;
	}
	lsr_read_coordinate(lsr, &elt->r, 1, "r");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_rect(GF_LASeRCodec *lsr, u32 same_type)
{
	SVGrectElement*elt = (SVGrectElement*) gf_node_new(lsr->sg, TAG_SVG_rect);

	if (same_type) {
		if (lsr->prev_rect) {
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_rect, (same_type==2) ? 1 : 0, 0);
			gf_mx2d_copy(elt->transform, lsr->prev_rect->transform);
		}
		lsr_read_id(lsr, (GF_Node *) elt);
		if (same_type==2) lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_coordinate(lsr, &elt->height, 0, "height");
		lsr_read_coordinate(lsr, &elt->width, 0, "width");
		lsr_read_coordinate(lsr, &elt->x, 1, "x");
		lsr_read_coordinate(lsr, &elt->y, 1, "y");
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_stroke(lsr, (SVGElement *) elt);
		lsr_read_coordinate(lsr, &elt->height, 0, "height");
		lsr_read_coordinate(lsr, &elt->rx, 1, "rx");
		lsr_read_coordinate(lsr, &elt->ry, 1, "ry");
		lsr_read_coordinate(lsr, &elt->width, 0, "width");
		lsr_read_coordinate(lsr, &elt->x, 1, "x");
		lsr_read_coordinate(lsr, &elt->y, 1, "y");
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_rect = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, same_type);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_script(GF_LASeRCodec *lsr)
{
	u32 flag;
	SVGscriptElement*elt = (SVGscriptElement*) gf_node_new(lsr->sg, TAG_SVG_script);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	/*TODO fix code generator*/
	lsr_read_single_time(lsr, NULL/*&elt->timing->begin*/, "begin");
	GF_LSR_READ_INT(lsr, flag/*elt->enabled*/, 1, "enabled");
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_content_type(lsr, &elt->xlink->type, "type");
	/*TODO fix code generator*/
	lsr_read_href(lsr, NULL /*&elt->xlink->href*/);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_command_list(lsr, NULL, elt);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_set(GF_LASeRCodec *lsr, SVGElement *parent)
{
	u32 flag;
	SVGsetElement*elt = (SVGsetElement*) gf_node_new(lsr->sg, TAG_SVG_set);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_time_list(lsr, elt->timing->begin, "_5_begin");
	lsr_read_duration(lsr, &elt->timing->dur, "_5_dur");
	/*TODO lsr:enabled not present in our code*/
	GF_LSR_READ_INT(lsr, flag/*elt->enabled*/, 1, "enabled");
	lsr_read_anim_fill(lsr, &elt->timing->fill, "_5_fill");
	lsr_read_anim_repeat(lsr, &elt->timing->repeatCount, "_5_repeatCount");
	lsr_read_repeat_duration(lsr, &elt->timing->repeatDur, "_5_repeatDur");
	lsr_read_anim_restart(lsr, &elt->timing->restart, "_5_restart");
	lsr_read_anim_value(lsr, &elt->anim->to, "_5_to");
	elt->anim->attributeName.type = lsr_read_animatable(lsr, "_5_attributeName");
	lsr_read_href(lsr, (SVGElement *)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);

	lsr_setup_attribute_name(lsr, (SVGElement *)elt, parent, &elt->anim->attributeName);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_stop(GF_LASeRCodec *lsr)
{
	SVGstopElement *elt = (SVGstopElement *) gf_node_new(lsr->sg, TAG_SVG_stop);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_fill(lsr, (SVGElement *) elt);
	lsr_read_stroke(lsr, (SVGElement *) elt);
	elt->offset.value = lsr_read_fixed_16_8(lsr, "offset");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}
static GF_Node *lsr_read_svg(GF_LASeRCodec *lsr)
{
	u32 flag;
	SVGsvgElement*elt = (SVGsvgElement*) gf_node_new(lsr->sg, TAG_SVG_svg);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_fill(lsr, (SVGElement *) elt);
	lsr_read_stroke(lsr, (SVGElement *) elt);
	lsr_read_string_attribute(lsr, &elt->baseProfile, "baseProfile");
	lsr_read_string_attribute(lsr, &elt->contentScriptType, "contentScriptType");
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_value_with_units(lsr, &elt->height, "height");
	GF_LSR_READ_INT(lsr, flag, 1, "hasPlaybackOrder");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "playbackOrder");
		if (flag) elt->playbackOrder=SVG_PLAYBACKORDER_FORWARDONLY;
	}
	GF_LSR_READ_INT(lsr, flag, 1, "hasPreserveAR");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 5, "preserveAR");
		if (flag % 2) {
			elt->preserveAspectRatio.meetOrSlice = SVG_MEETORSLICE_MEET;
			flag --;
		}
		switch (flag) {
		case 1: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMAXYMAX; break;
		case 3: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMAXYMID; break;
		case 5: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMAXYMIN; break;
		case 7: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMIDYMAX; break;
		case 9: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMIDYMID; break;
		case 11: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMIDYMIN; break;
		case 13: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMINYMAX; break;
		case 15: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMINYMID; break;
		case 17: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_XMINYMIN; break;
		default: elt->preserveAspectRatio.align = SVG_PRESERVEASPECTRATIO_NONE; break;
		}
	}
	GF_LSR_READ_INT(lsr, flag, 1, "hasSyncBehavior");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 2, "syncBehavior");
		switch (flag) {
		case 0: elt->sync->syncBehaviorDefault = SMIL_SYNCBEHAVIOR_CANSLIP; break;
		case 1: elt->sync->syncBehaviorDefault = SMIL_SYNCBEHAVIOR_INDEPENDENT; break;
		case 3: elt->sync->syncBehaviorDefault = SMIL_SYNCBEHAVIOR_LOCKED; break;
		default: elt->sync->syncBehaviorDefault = 0; break;
		}
	}
	GF_LSR_READ_INT(lsr, flag, 1, "hasSyncTolerance");
	if (flag) {
		elt->sync->syncToleranceDefault.type = SMIL_SYNCTOLERANCE_VALUE;
		GF_LSR_READ_INT(lsr, flag, 1, "choice");
	    elt->sync->syncToleranceDefault.value = lsr_read_vluimsbf5(lsr, "value");
		elt->sync->syncToleranceDefault.value /= lsr->time_resolution;
	}
	GF_LSR_READ_INT(lsr, flag, 1, "hasTimelineBegin");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "timelineBegin");
		if (flag) elt->timelineBegin = SVG_TIMELINEBEGIN_ONLOAD;
	}
	lsr_read_string_attribute(lsr, &elt->version, "version");

	GF_LSR_READ_INT(lsr, flag, 1, "hasViewBox");
	if (flag) {
		elt->viewBox.x = lsr_read_fixed_16_8(lsr, "viewbox.x");
		elt->viewBox.y = lsr_read_fixed_16_8(lsr, "viewbox.y");
		elt->viewBox.width = lsr_read_fixed_16_8(lsr, "viewbox.width");
		elt->viewBox.height = lsr_read_fixed_16_8(lsr, "viewbox.height");
	}
	lsr_read_value_with_units(lsr, &elt->width, "width");
	/*zoom and pan must be encoded in our code...*/
	GF_LSR_READ_INT(lsr, flag, 1, "hasZoomAndPan");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "zoomAndPan");
		elt->zoomAndPan = flag ? SVG_ZOOMANDPAN_MAGNIFY : SVG_ZOOMANDPAN_DISABLE;
	}
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_switch(GF_LASeRCodec *lsr)
{
	SVGswitchElement*elt = (SVGswitchElement*) gf_node_new(lsr->sg, TAG_SVG_switch);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
	lsr_read_fill(lsr, (SVGElement*)elt);
	lsr_read_stroke(lsr, (SVGElement*)elt);
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_text(GF_LASeRCodec *lsr, u32 same_type)
{
	SVGtextElement*elt = (SVGtextElement*) gf_node_new(lsr->sg, TAG_SVG_text);
	if (same_type) {
		if (lsr->prev_text) {
			GF_FieldInfo f1, f2;
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_text, (same_type==2) ? 1 : 0, 0);
			gf_mx2d_copy(elt->transform, lsr->prev_text->transform);
			elt->editable = lsr->prev_text->editable;
			/*restore rotate*/
			f1.fieldType = f2.fieldType = SVG_Numbers_datatype;
			f1.far_ptr = elt->rotate;
			f2.far_ptr = lsr->prev_text->rotate;
			svg_attributes_copy(&f1, &f2, 0);
		}
		lsr_read_id(lsr, (GF_Node *) elt);
		if (same_type==2) lsr_read_fill(lsr, (SVGElement *) elt);
		lsr_read_coord_list(lsr, elt->x, "x");
		lsr_read_coord_list(lsr, elt->y, "y");
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement*)elt);
		lsr_read_stroke(lsr, (SVGElement*)elt);
		GF_LSR_READ_INT(lsr, elt->editable, 1, "editable");
		lsr_read_float_list(lsr, elt->rotate, "rotate");
		lsr_read_coord_list(lsr, elt->x, "x");
		lsr_read_coord_list(lsr, elt->y, "y");
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_text = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, same_type);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_tspan(GF_LASeRCodec *lsr)
{
	SVGtspanElement*elt = (SVGtspanElement*) gf_node_new(lsr->sg, TAG_SVG_tspan);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_fill(lsr, (SVGElement*)elt);
	lsr_read_stroke(lsr, (SVGElement*)elt);
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_use(GF_LASeRCodec *lsr, Bool is_same)
{
	u32 flag;
	SVGuseElement*elt = (SVGuseElement*) gf_node_new(lsr->sg, TAG_SVG_use);
	if (is_same) {
		if (lsr->prev_use) {
			lsr_restore_base(lsr, (SVGElement *)elt, (SVGElement *)lsr->prev_use, 0, 0);
			gf_mx2d_copy(elt->transform, lsr->prev_use->transform);
			elt->core->eRR = lsr->prev_use->core->eRR;
			elt->x = lsr->prev_use->x;
			elt->y = lsr->prev_use->y;
			/*TODO restore overflow*/
		}
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_href(lsr, (SVGElement *)elt);
	} else {
		lsr_read_string_attribute(lsr, &elt->core->_class, "class");
		lsr_read_id(lsr, (GF_Node *) elt);
		lsr_read_rare_full(lsr, (SVGElement*) elt, &elt->transform);
		lsr_read_fill(lsr, (SVGElement*)elt);
		lsr_read_stroke(lsr, (SVGElement*)elt);
		GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
		/*TODO */
		GF_LSR_READ_INT(lsr, flag, 1, "hasOverflow");
		lsr_read_coordinate(lsr, &elt->x, 1, "x");
		lsr_read_coordinate(lsr, &elt->y, 1, "y");
		lsr_read_href(lsr, (SVGElement *)elt);
		lsr_read_any_attribute(lsr, (GF_Node *) elt);
		lsr->prev_use = elt;
	}
	lsr_read_group_content(lsr, (SVGElement *) elt, is_same);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_video(GF_LASeRCodec *lsr, SVGElement *parent)
{
	u32 flag;
	u8 fixme;
	SVGvideoElement*elt = (SVGvideoElement*) gf_node_new(lsr->sg, TAG_SVG_video);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement*) elt);
	lsr_read_time_list(lsr, elt->timing->begin, "_5_begin");
	lsr_read_duration(lsr, &elt->timing->dur, "_5_dur");
	GF_LSR_READ_INT(lsr, elt->core->eRR, 1, "externalResourcesRequired");
	lsr_read_coordinate(lsr, &elt->height, 1, "height");
	/*TODO*/
	GF_LSR_READ_INT(lsr, flag, 1, "hasOverlay");
	lsr_read_anim_repeat(lsr, &elt->timing->repeatCount, "_5_repeatCount");
	lsr_read_repeat_duration(lsr, &elt->timing->repeatDur, "_5_repeatDur");
	lsr_read_sync_behavior(lsr, &elt->sync->syncBehavior, "syncBehavior");
	lsr_read_sync_tolerance(lsr, &elt->sync->syncTolerance, "syncBehavior");
	lsr_read_transform_behavior(lsr, &fixme, "transformBehavior");
	lsr_read_content_type(lsr, &elt->xlink->type, "type");
	lsr_read_coordinate(lsr, &elt->width, 1, "width");
	lsr_read_coordinate(lsr, &elt->x, 1, "x");
	lsr_read_coordinate(lsr, &elt->y, 1, "y");
	lsr_read_href(lsr, (SVGElement *)elt);
//	lsr_read_time(lsr, elt->clipBegin, "clipBegin");
	GF_LSR_READ_INT(lsr, flag, 1, "clipBegin");
//	lsr_read_time(lsr, elt->clipEnd, "clipEnd");
	GF_LSR_READ_INT(lsr, flag, 1, "clipEnd");
//	lsr_read_href(lsr, & elt->syncReference);
	GF_LSR_READ_INT(lsr, flag, 1, "hasSyncReference");
	
	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_listener(GF_LASeRCodec *lsr)
{
	u32 flag;
	SVGlistenerElement*elt = (SVGlistenerElement*) gf_node_new(lsr->sg, TAG_SVG_listener);
	lsr_read_string_attribute(lsr, &elt->core->_class, "class");
	lsr_read_id(lsr, (GF_Node *) elt);
	lsr_read_rare(lsr, (SVGElement *) elt);
	GF_LSR_READ_INT(lsr, elt->defaultAction, 1, "hasDefaultAction");
	if (elt->defaultAction) GF_LSR_READ_INT(lsr, elt->defaultAction, 1, "defaultAction");
	GF_LSR_READ_INT(lsr, flag/*elt->endabled*/, 1, "enabled");
	GF_LSR_READ_INT(lsr, flag, 1, "hasEvent");
	if (flag) lsr_read_event_type(lsr, &elt->event, NULL);
	GF_LSR_READ_INT(lsr, flag, 1, "hasHandler");
	if (flag) lsr_read_any_uri(lsr, &elt->handler, "handler");
	GF_LSR_READ_INT(lsr, flag, 1, "hasObserver");
	/*TODO double check spec here*/
	if (flag) lsr_read_codec_IDREF(lsr, &elt->observer, "observer");
	GF_LSR_READ_INT(lsr, flag, 1, "hasPropagate");
	if (flag) {
		GF_LSR_READ_INT(lsr, flag/*elt->prop*/, 1, "propagate");
	}
	GF_LSR_READ_INT(lsr, flag, 1, "hasTarget");
	if (flag) lsr_read_codec_IDREF(lsr, &elt->target, "target");
	/*TODO modify our SVG tree for LASeR*/
	GF_LSR_READ_INT(lsr, flag, 1, "hasDelay");
	/*TODO modify our SVG tree for LASeR*/
	GF_LSR_READ_INT(lsr, flag, 1, "hasTimeAttribute");

	lsr_read_any_attribute(lsr, (GF_Node *) elt);
	lsr_read_group_content(lsr, (SVGElement *) elt, 0);
	return (GF_Node *)elt;
}

static GF_Node *lsr_read_content_model_2(GF_LASeRCodec *lsr, SVGElement *parent)
{
	GF_Node *n;
	u32 ntype;
	GF_LSR_READ_INT(lsr, ntype, 6, "ch4"); 
	n = NULL;
	switch (ntype) {
	case 0: n = lsr_read_a(lsr); break;
	case 1: n = lsr_read_animate(lsr, parent); break;
	case 2: n = lsr_read_animate(lsr, parent); break;
	case 3: n = lsr_read_animateMotion(lsr, parent); break;
	case 4: n = lsr_read_animateTransform(lsr, parent); break;
	case 5: n = lsr_read_audio(lsr, parent); break;
	case 6: n = lsr_read_circle(lsr); break;
#if TODO_LASER_EXTENSIONS	
	case 7: n = lsr_read_cursor(lsr); break;
#endif
	case 8: n = lsr_read_defs(lsr); break;
	case 9: n = lsr_read_data(lsr, TAG_SVG_desc); break;
	case 10: n = lsr_read_ellipse(lsr); break;
	case 11: n = lsr_read_foreignObject(lsr); break;
	case 12: n = lsr_read_g(lsr, 0); break;
	case 13: n = lsr_read_image(lsr); break;
	case 14: n = lsr_read_line(lsr, 0); break;
	case 15: n = lsr_read_linearGradient(lsr); break;
	case 16: n = lsr_read_data(lsr, TAG_SVG_metadata); break;
	case 17: n = lsr_read_mpath(lsr); break;
	case 18: n = lsr_read_path(lsr, 0); break;
	case 19: n = lsr_read_polygon(lsr, 0, 0); break;
	case 20: n = lsr_read_polygon(lsr, 1, 0); break;
	case 21: n = lsr_read_radialGradient(lsr); break;
	case 22: n = lsr_read_rect(lsr, 0); break;
	case 23: n = lsr_read_g(lsr, 1); break;
	case 24: n = lsr_read_line(lsr, 1); break;
	case 25: n = lsr_read_path(lsr, 1); break;
	case 26: n = lsr_read_path(lsr, 2); break;
	case 27: n = lsr_read_polygon(lsr, 0, 1); break;
	case 28: n = lsr_read_polygon(lsr, 0, 2); break;
	case 29: n = lsr_read_polygon(lsr, 0, 3); break;
	case 30: n = lsr_read_polygon(lsr, 1, 1); break;
	case 31: n = lsr_read_polygon(lsr, 1, 2); break;
	case 32: n = lsr_read_polygon(lsr, 1, 3); break;
	case 33: n = lsr_read_rect(lsr, 1); break;
	case 34: n = lsr_read_rect(lsr, 2); break;
	case 35: n = lsr_read_text(lsr, 1); break;
	case 36: n = lsr_read_text(lsr, 2); break;
	case 37: n = lsr_read_use(lsr, 1); break;
	case 38: n = lsr_read_script(lsr); break;
	case 39: n = lsr_read_set(lsr, parent); break;
	case 40: n = lsr_read_stop(lsr); break;
	case 41: n = lsr_read_switch(lsr); break;
	case 42: n = lsr_read_text(lsr, 0); break;
	case 43: n = lsr_read_data(lsr, TAG_SVG_title); break;
	case 44: n = lsr_read_tspan(lsr); break;
	case 45: n = lsr_read_use(lsr, 0); break;
	case 46: n = lsr_read_video(lsr, parent); break;
	case 47: n = lsr_read_listener(lsr); break;
	case 48: 
		lsr_read_extend_class(lsr, NULL, 0, "node"); 
		break;
	case 49: 
		lsr_read_private_element_container(lsr); 
		break;
	case 50:
		lsr_read_byte_align_string(lsr, &parent->textContent, "textContent");
		break;
	default:
		break;
	}
	return n;
}

static GF_Node *lsr_read_content_model_36(GF_LASeRCodec *lsr, SVGElement *parent)
{
	u32 flag;
	GF_Node *n=NULL;
	GF_LSR_READ_INT(lsr, flag, 1, "ch4"); 
	if (flag) {
		GF_LSR_READ_INT(lsr, flag, 1, "ch6"); 
		if (flag) {
			lsr_read_extend_class(lsr, NULL, 0, "extend");
		} else {
			lsr_read_private_element_container(lsr);
		}
		return NULL;
	}
	GF_LSR_READ_INT(lsr, flag, 6, "ch6");
	switch(flag) {
	case 0: n = lsr_read_a(lsr); break;
	case 1: n = lsr_read_animate(lsr, parent); break;
	case 2: n = lsr_read_animate(lsr, parent); break;
	case 3: n = lsr_read_animateMotion(lsr, parent); break;
	case 4: n = lsr_read_animateTransform(lsr, parent); break;
	case 5: n = lsr_read_audio(lsr, parent); break;
	case 6: n = lsr_read_circle(lsr); break;
#if TODO_LASER_EXTENSIONS	
	case 7: n = lsr_read_cursor(lsr); break;
#endif
	case 8: n = lsr_read_defs(lsr); break;
	case 9: n = lsr_read_data(lsr, TAG_SVG_desc); break;
	case 10: n = lsr_read_ellipse(lsr); break;
	case 11: n = lsr_read_foreignObject(lsr); break;
	case 12: n = lsr_read_g(lsr, 0); break;
	case 13: n = lsr_read_image(lsr); break;
	case 14: n = lsr_read_line(lsr, 0); break;
	case 15: n = lsr_read_linearGradient(lsr); break;
	case 16: n = lsr_read_data(lsr, TAG_SVG_metadata); break;
	case 17: n = lsr_read_mpath(lsr); break;
	case 18: n = lsr_read_path(lsr, 0); break;
	case 19: n = lsr_read_polygon(lsr, 0, 0); break;
	case 20: n = lsr_read_polygon(lsr, 1, 0); break;
	case 21: n = lsr_read_radialGradient(lsr); break;
	case 22: n = lsr_read_rect(lsr, 0); break;
	case 23: n = lsr_read_script(lsr); break;
	case 24: n = lsr_read_set(lsr, parent); break;
	case 25: n = lsr_read_stop(lsr); break;
	case 26: n = lsr_read_svg(lsr); break;
	case 27: n = lsr_read_switch(lsr); break;
	case 28: n = lsr_read_text(lsr, 0); break;
	case 29: n = lsr_read_data(lsr, TAG_SVG_title); break;
	case 30: n = lsr_read_tspan(lsr); break;
	case 31: n = lsr_read_use(lsr, 0); break;
	case 32: n = lsr_read_video(lsr, parent); break;
	case 33: n = lsr_read_listener(lsr); break;
	}
	return n;
}

static void lsr_read_group_content(GF_LASeRCodec *lsr, SVGElement *elt, Bool skip_object_content)
{
	u32 i, count;
	if (!skip_object_content) lsr_read_object_content(lsr, elt);
	/*node attributes are all parsed*/
	gf_node_init((GF_Node *)elt);

	GF_LSR_READ_INT(lsr, count, 1, "opt_group");
	if (count) {
		count = lsr_read_vluimsbf5(lsr, "occ0");
		for (i=0; i<count; i++) {
			GF_Node *n = lsr_read_content_model_2(lsr, elt);
			if (n) {
				gf_node_register(n, (GF_Node *)elt);
				gf_list_add(elt->children, n);
				if (lsr->trace) fprintf(lsr->trace, "//end %s\n", gf_node_get_class_name(n));
			} else {
				/*either error or text content*/
			}
		}
	}
	/*fire OnLoad event*/
}



static void lsr_read_update_value(GF_LASeRCodec *lsr, GF_Node *node, u32 fieldType, void *val, Bool is_indexed)
{
	u32 is_inherit, has_escape, escape_val;
	SVG_Paint *paint;
	SVG_Number *n;
	is_inherit = has_escape = 0;

	GF_LSR_READ_INT(lsr, is_inherit, 1, "isInherit"); 

	if (is_indexed) {
	} else {
		GF_LSR_READ_INT(lsr, has_escape, 1, "escapeFlag"); 
		if (has_escape) GF_LSR_READ_INT(lsr, escape_val, 2, "escapeEnum"); 

		switch (fieldType) {
		case SVG_Boolean_datatype:
			if (!is_inherit && !has_escape) GF_LSR_READ_INT(lsr, *(SVG_Boolean*)val, 1, "val"); 
			break;
		case SVG_Paint_datatype:
			paint = (SVG_Paint *)val;
			if (is_inherit) paint->type = SVG_PAINT_INHERIT;
			else if (!has_escape) lsr_read_paint(lsr, val, "val"); 
			break;
		case SVG_Opacity_datatype:
		case SVG_AudioLevel_datatype:
			n = val;
			if (is_inherit) n->type=SVG_NUMBER_INHERIT;
			else if (!has_escape) {
				n->type = SVG_NUMBER_VALUE;
				n->value = lsr_read_fixed_clamp(lsr, "val");
			}
			break;
		case SVG_Matrix_datatype:
			if (!is_inherit && !has_escape) lsr_read_matrix(lsr, val);
			break;
		case SVG_Number_datatype:
		case SVG_StrokeMiterLimit_datatype:
		case SVG_FontSize_datatype:
		case SVG_StrokeDashOffset_datatype:
		case SVG_StrokeWidth_datatype:
		case SVG_Length_datatype:
		case SVG_Coordinate_datatype:
			n = val;
			if (is_inherit) n->type=SVG_NUMBER_INHERIT;
			else if (!has_escape) {
				n->type = SVG_NUMBER_VALUE;
				n->value = lsr_read_fixed_16_8(lsr, "val");
			}
			break;
		case SVG_LineIncrement_datatype:
			n = val;
			if (is_inherit) n->type=SVG_NUMBER_INHERIT;
			else if (has_escape) n->type = SVG_NUMBER_AUTO;
			else {
				n->type = SVG_NUMBER_VALUE;
				n->value = lsr_read_fixed_16_8(lsr, "line-increment-value");
			}
			break;
		case SVG_Rotate_datatype:
			n = val;
			if (is_inherit) n->type=SVG_NUMBER_INHERIT;
			else if (has_escape) n->type = escape_val ? SVG_NUMBER_AUTO_REVERSE : SVG_NUMBER_AUTO;
			else {
				n->type = SVG_NUMBER_VALUE;
				n->value = lsr_read_fixed_16_8(lsr, "rotate");
			}
			break;
		case SVG_NumberOrPercentage_datatype:
			n = val;
			if (is_inherit) n->type = SVG_NUMBER_INHERIT;
			else if (!has_escape) lsr_read_value_with_units(lsr, val, "val");
			break;
		case SVG_Coordinates_datatype:
			if (!is_inherit && !has_escape) lsr_read_float_list(lsr, *(GF_List **)val, "val");
			break;
		case SVG_IRI_datatype:
			if (!is_inherit && !has_escape) lsr_read_any_uri(lsr, val, "val");
			break;

		case SVG_String_datatype:
		case SVG_TextContent_datatype:
		case SVG_ContentType_datatype:
		case SVG_LinkTarget_datatype:
		case SVG_LanguageID_datatype:
			if (!is_inherit && !has_escape) lsr_read_byte_align_string(lsr, val, "val");
			break;
		case SVG_Motion_datatype:
			if (!is_inherit && !has_escape) {
				((SVG_Point *)val)->x = lsr_read_fixed_16_8(lsr, "val_x");
				((SVG_Point *)val)->y = lsr_read_fixed_16_8(lsr, "val_y");
			}
			break;
		case SVG_Points_datatype:
			if (!is_inherit && !has_escape) lsr_read_point_sequence(lsr, *(GF_List **)val, "val");
			break;
		case SVG_PathData_datatype:
			if (!is_inherit && !has_escape) lsr_read_path_type(lsr, val, "val");
			break;
		default:
			if ((fieldType>=SVG_FillRule_datatype) && (fieldType<=SVG_TransformBehavior_datatype)) {
				/*TODO fixme, check inherit values*/
				if (is_inherit) *(u8 *)val = 0;
				else if (!has_escape) *(u8 *)val = lsr_read_vluimsbf5(lsr, "val");
			} else {
				fprintf(stdout, "Warning: update value not supported\n");
			}
		}
	}
	if (node) gf_node_dirty_set(node, 0, 0);
}

static s32 lsr_get_field_from_attrib_type(GF_Node *n, u32 att_type)
{
	u32 i, count = gf_node_get_field_count(n);
	for (i=0; i<count; i++) {
		s32 type = gf_lsr_field_to_attrib_type(n, i);
		if ((u32)type == att_type) return i;
	}
	return -1;
}

static GF_Err lsr_read_add_replace_insert(GF_LASeRCodec *lsr, GF_List *com_list, u32 com_type)
{
	GF_FieldInfo info;
	GF_Node *n, *operandNode, *new_node;
	GF_Command *com;
	GF_CommandField *field;
	s32 idx, att_type, op_att_type;
	u32 type, idref, op_idref;

	operandNode = NULL;
	att_type = op_att_type = -1;
	GF_LSR_READ_INT(lsr, type, 1, "has_attributeName");
	if (type) GF_LSR_READ_INT(lsr, att_type, 8, "attributeName");
	idx = -1;
	if (com_type) {
		GF_LSR_READ_INT(lsr, type, 1, "has_index");
		if (type) idx = lsr_read_vluimsbf5(lsr, "index");
	}
	if (com_type!=3) {
		GF_LSR_READ_INT(lsr, type, 1, "has_operandAttribute");
		if (type) GF_LSR_READ_INT(lsr, op_att_type, 8, "attributeName");
		GF_LSR_READ_INT(lsr, type, 1, "has_operandElementId");
		if (type) {
			op_idref = 1 + lsr_read_vluimsbf5(lsr, "operandElementId");
			operandNode = gf_sg_find_node(lsr->sg, op_idref);
			if (!operandNode) return GF_NON_COMPLIANT_BITSTREAM;
		}
	}
	GF_LSR_READ_INT(lsr, type, 1, "hasID");
	n = NULL;
	if (type) {
		idref = 1 + lsr_read_vluimsbf5(lsr, "ref");
		n = gf_sg_find_node(lsr->sg, idref);
	}
	if (!n)  return GF_NON_COMPLIANT_BITSTREAM;
	GF_LSR_READ_INT(lsr, type, 1, "has_value");
	if (type) {
		/*node or node-list replacement*/
		if (att_type<0) {
			GF_Node *new_node;
			if (!com_type) return GF_NON_COMPLIANT_BITSTREAM;
			GF_LSR_READ_INT(lsr, type, 1, "isInherit");
			if (type) return GF_NON_COMPLIANT_BITSTREAM;
			if (idx==-1) {
				GF_LSR_READ_INT(lsr, type, 1, "escapeFlag");
				if (type) return GF_NON_COMPLIANT_BITSTREAM;
			}

			new_node = lsr_read_content_model_36(lsr, (idx==-1) ? NULL : (SVGElement *)n);
			if (com_list) {
				com = gf_sg_command_new(lsr->sg, (com_type==3) ? GF_SG_LSR_INSERT : GF_SG_LSR_REPLACE);
				gf_list_add(com_list, com);
				com->node = n;
				gf_node_register(com->node, NULL);
				field = gf_sg_command_field_new(com);
				field->pos = idx;
				field->new_node = new_node ;
				gf_node_register(new_node, NULL);
			} else if (com_type==3) {
				gf_list_insert(((SVGElement *)n)->children, new_node, idx);
				gf_node_register(new_node, n);
			} else {
				/*child replacement*/
				if (idx!=-1) {
					GF_Node *old = gf_list_get( ((SVGElement *)n)->children, idx);
					if (old)
						gf_node_replace(old, new_node, 0);
					else {
						gf_list_add(((SVGElement *)n)->children, new_node);
						gf_node_register(new_node, n);
					}
				} else {
					/*node replacement*/
					gf_node_replace(n, new_node, 0);
				}
			}
		}
		/*value replace/add*/
		else if (com_list) {
			com = gf_sg_command_new(lsr->sg, (com_type==0) ? GF_SG_LSR_ADD : (com_type==3) ? GF_SG_LSR_INSERT : GF_SG_LSR_REPLACE);
			gf_list_add(com_list, com);
			com->node = n;
			gf_node_register(com->node, NULL);
			field = gf_sg_command_field_new(com);
			field->pos = idx;
			if (att_type==102) {
				field->fieldIndex = (u32) -1;
				field->fieldType = SVG_String_datatype;
			} else {
				field->fieldIndex = lsr_get_field_from_attrib_type(com->node, att_type);
				gf_node_get_field(com->node, field->fieldIndex, &info);
				field->fieldType = info.fieldType;
			}
			field->field_ptr = svg_create_value_from_attributetype(field->fieldType, 0);
			lsr_read_update_value(lsr, NULL, field->fieldType, field->field_ptr, (idx==-1) ? 0 : 1);
		} else {
			if (att_type==102) {
				info.far_ptr = &((SVGElement *)n)->textContent;
				info.fieldType = SVG_String_datatype;
				info.name = "textContent";
			} else {
				u32 fieldIndex = lsr_get_field_from_attrib_type((GF_Node*)n, att_type);
				gf_node_get_field(n, fieldIndex, &info);
			}
			if (com_type) {
				lsr_read_update_value(lsr, (GF_Node*)n, info.fieldType, info.far_ptr, (idx==-1) ? 0 : 1);
			} else {
				GF_FieldInfo tmp;
				tmp = info;
				tmp.far_ptr = svg_create_value_from_attributetype(info.fieldType, 0);
				lsr_read_update_value(lsr, n, info.fieldType, tmp.far_ptr, (idx==-1) ? 0 : 1);
				svg_attributes_add(&info, &tmp, &info, 0);
				gf_svg_delete_attribute_value(info.fieldType, tmp.far_ptr);
			}
		}
	} 
	/*copy from node*/
	else if (operandNode && (op_att_type>=0)) {
		u32 opFieldIndex = lsr_get_field_from_attrib_type(operandNode, op_att_type);
		if (com_list) {
			com = gf_sg_command_new(lsr->sg, com_type ? GF_SG_LSR_REPLACE : GF_SG_LSR_ADD);
			gf_list_add(com_list, com);
			com->node = n;
			gf_node_register(com->node, NULL);
			com->fromNodeID = op_idref;
			com->fromFieldIndex = opFieldIndex;
			field = gf_sg_command_field_new(com);
			field->pos = idx;
			field->fieldIndex = lsr_get_field_from_attrib_type(com->node, att_type);
		} else {
			u32 fieldIndex;
			GF_FieldInfo op_info;
			fieldIndex = lsr_get_field_from_attrib_type(n, att_type);
			gf_node_get_field(n, fieldIndex, &info);
			gf_node_get_field(operandNode, opFieldIndex, &op_info);
			if (com_type) {
				svg_attributes_copy(&info, &op_info, 0);
			} else {
				svg_attributes_add(&info, &op_info, &info, 0);
			}
		}
	}

	lsr_read_any_attribute(lsr, NULL);

	/*list replacement*/
	GF_LSR_READ_INT(lsr, type, 1, "opt_group");
	if (type) {
		u32 count;
		GF_List *nlist;
		GF_Node *par;
		if (!com_type || (idx!=-1) ) return GF_NON_COMPLIANT_BITSTREAM;

		if (com_list) {
			com = gf_sg_command_new(lsr->sg, (com_type==5) ? GF_SG_LSR_REPLACE : GF_SG_LSR_INSERT);
			gf_list_add(com_list, com);
			com->node = n;
			gf_node_register(com->node, NULL);
			field = gf_sg_command_field_new(com);
			field->node_list = gf_list_new();
			field->field_ptr = &field->node_list;
			nlist = field->node_list;
			par = NULL;
		} else {
			nlist = ((SVGElement*)n)->children;
			gf_node_unregister_children(n, nlist);
			par = n;
		}
		count = lsr_read_vluimsbf5(lsr, "count");
		while (count) {
			count--;
			new_node = lsr_read_content_model_36(lsr, (SVGElement *) n);
			if (new_node) {
				gf_list_add(nlist, new_node);
				gf_node_register(new_node, par);
			}
		}
		if (par) gf_node_changed(n, NULL);
	}
	return GF_OK;
}

static GF_Err lsr_read_delete(GF_LASeRCodec *lsr, GF_List *com_list)
{
	GF_Command *com;
	GF_CommandField *field;
	s32 idx;
	u32 type, idref;

	idx = -1;
	GF_LSR_READ_INT(lsr, type, 1, "has_index");
	if (type) idx = (s32) lsr_read_vluimsbf5(lsr, "index");
	idref = 1 + lsr_read_vluimsbf5(lsr, "ref");
	lsr_read_any_attribute(lsr, NULL);
	if (com_list) {
		com = gf_sg_command_new(lsr->sg, GF_SG_LSR_DELETE);
		com->node = gf_sg_find_node(lsr->sg, idref);
		if (!com->node) return GF_NON_COMPLIANT_BITSTREAM;
		gf_node_register(com->node, NULL);
		gf_list_add(com_list, com);
		if (idx>=0) {
			field = gf_sg_command_field_new(com);
			field->pos = idx;
		}
	} else {
		SVGElement *elt = (SVGElement *) gf_sg_find_node(lsr->sg, idref);
		if (!elt) return GF_NON_COMPLIANT_BITSTREAM;
		if (idx>=0) {
			GF_Node *c = gf_list_get(elt->children, idx);
			if (c) {
				GF_Err e = gf_list_rem(elt->children, idx);
				if (e) 
					return e;
				gf_node_unregister(c, (GF_Node*)elt);
			}
		} else {
			gf_node_replace((GF_Node*)elt, NULL, 0);
		}
	}
	return GF_OK;
}

static GF_Err lsr_read_command_list(GF_LASeRCodec *lsr, GF_List *com_list, SVGscriptElement *script)
{
	GF_Node *n;
	GF_Command *com;
	GF_Err e;
	u32 i, type, count = 0;
	if (com_list) count += gf_list_count(com_list);
	if (script && script->textContent) count += 1;

	if (script) {
		lsr_read_object_content(lsr, (SVGElement *) script);
		GF_LSR_READ_INT(lsr, count, 1, "opt_group");
		if (!count) {
			gf_node_init((GF_Node *)script);
			return GF_OK;
		}
	}
	count = lsr_read_vluimsbf5(lsr, "occ0");
	for (i=0; i<count; i++) {
		GF_LSR_READ_INT(lsr, type, 4, "ch4");
		switch (type) {
		case 0:	/*add*/
		case 3:	/*insert*/
		case 5:	/*replace*/
			e = lsr_read_add_replace_insert(lsr, com_list, type);
			break;
		case 2: /*Delete*/
			e = lsr_read_delete(lsr, com_list);
			break;
		case 4: /*NewScene*/
			lsr_read_any_attribute(lsr, NULL);
			if (com_list) {
				n = lsr_read_svg(lsr);
				if (!n) return GF_NON_COMPLIANT_BITSTREAM;
				gf_node_register(n, NULL);
				com = gf_sg_command_new(lsr->sg, GF_SG_LSR_NEW_SCENE);
				com->node = n;
				gf_list_add(com_list, com);
			} else {
				gf_sg_reset(lsr->sg);
				n = lsr_read_svg(lsr);
				if (!n) return GF_NON_COMPLIANT_BITSTREAM;
				gf_node_register(n, NULL);
				gf_sg_set_root_node(lsr->sg, n);
				gf_sg_set_scene_size_info(lsr->sg, 0, 0, 1);
			}
			break;
		case 10:	/*script*/
			if (script) lsr_read_byte_align_string(lsr, &script->textContent, "textContent");
			break;
		default:
			return GF_NON_COMPLIANT_BITSTREAM;
		}
		/*same-coding scope is command-based (to check in the spec)*/
		if (!script) {
			lsr->prev_g = NULL;
			lsr->prev_line = NULL;
			lsr->prev_path = NULL;
			lsr->prev_polygon = NULL;
			lsr->prev_rect = NULL;
			lsr->prev_text = NULL;
			lsr->prev_use = NULL;
		}
	}
	return GF_OK;
}

static GF_Err lsr_decode_laser_unit(GF_LASeRCodec *lsr, GF_List *com_list)
{
	GF_Err e;
	Bool reset_encoding_context;
	u32 flag, i, count, privateDataIdentifierIndexBits;
	/*laser unit header*/
	GF_LSR_READ_INT(lsr, reset_encoding_context, 1, "resetEncodingContext");
	GF_LSR_READ_INT(lsr, flag, 1, "opt_group");
	if (flag) lsr_read_extension(lsr, "ext");

	/*codecInitialisations*/

	/*clean all tables*/
	if (reset_encoding_context) {
		lsr->nb_cols = 0;
		if (lsr->col_table) free(lsr->col_table);
		lsr->col_table = NULL;
		while (gf_list_count(lsr->font_table)) {
			char *ft = gf_list_last(lsr->font_table);
			free(ft);
			gf_list_rem_last(lsr->font_table);
		}
		lsr->privateData_id_index = lsr->privateTag_index = 0;
	}

	/*codec initializations*/
	/*1- color*/
	GF_LSR_READ_INT(lsr, flag, 1, "colorInitialisation");
	if (flag) {
		count = lsr_read_vluimsbf5(lsr, "count");
		lsr->col_table = realloc(lsr->col_table, sizeof(LSRCol)*(lsr->nb_cols+count));
		for (i=0; i<count; i++) {
			LSRCol c;
			GF_LSR_READ_INT(lsr, c.r, lsr->info->cfg.colorComponentBits, "red");
			GF_LSR_READ_INT(lsr, c.g, lsr->info->cfg.colorComponentBits, "green");
			GF_LSR_READ_INT(lsr, c.b, lsr->info->cfg.colorComponentBits, "blue");
			lsr->col_table[lsr->nb_cols+i] = c;
		}
		lsr->nb_cols += count;
	}
	lsr->colorIndexBits = gf_get_bit_size(lsr->nb_cols);
	/*2 - fonts*/
	GF_LSR_READ_INT(lsr, flag, 1, "fontInitialisation");
	if (flag) {
		count = lsr_read_vluimsbf5(lsr, "count");
		for (i=0; i<count; i++) {
			unsigned char *ft = NULL;
			lsr_read_byte_align_string(lsr, &ft, "font");
			gf_list_add(lsr->font_table, ft);
		}
	}
	lsr->fontIndexBits = gf_get_bit_size(count);
	/*3 - private*/
	GF_LSR_READ_INT(lsr, flag, 1, "privateDataIdentifierInitialisation");
	if (flag) {
		count = lsr_read_vluimsbf5(lsr, "nbPrivateDataIdentifiers");
		for (i=0; i<count; i++) {
			lsr->privateData_id_index++;
			lsr_read_byte_align_string(lsr, NULL, "privateDataIdentifier");
		}
	}
	/*4 - anyXML*/
	GF_LSR_READ_INT(lsr, flag, 1, "anyXMLInitialisation");
	if (flag) {
		privateDataIdentifierIndexBits = gf_get_bit_size(lsr->privateData_id_index);
		count = lsr_read_vluimsbf5(lsr, "nbTags");
		for (i=0; i<count; i++) {
			lsr->privateTag_index++;
			if (i) {
				/* uint(privateDataIdentifierIndexBits) = */
				GF_LSR_READ_INT(lsr, flag, privateDataIdentifierIndexBits, "privateDataIdentifierIndex");
				lsr_read_byte_align_string(lsr, NULL, "tag");
			}
			GF_LSR_READ_INT(lsr, flag, 1, "hasAttrs");
			if (flag) {
		        u32 k, c2 = lsr_read_vluimsbf5(lsr, "nbAttrNames");
				for (k=0; k<c2; k++) {
					if (!i) {
						/* uint(privateDataIdentifierIndexBits) = */lsr_read_vluimsbf5(lsr, "privateDataIdentifierIndex");
						GF_LSR_READ_INT(lsr, flag, privateDataIdentifierIndexBits, "privateDataIdentifierIndex");
					}
					lsr_read_byte_align_string(lsr, NULL, "tag");
				}
			}
		}
	}
	/*5 - extended*/
	GF_LSR_READ_INT(lsr, flag, 1, "extendedInitialisation");
	if (flag) lsr_read_extension(lsr, "c");

	e = lsr_read_command_list(lsr, com_list, NULL);
	GF_LSR_READ_INT(lsr, flag, 1, "opt_group");
	if (0) lsr_read_extension(lsr, "ext");
	return e;
}

