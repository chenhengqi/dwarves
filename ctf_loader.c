/* ctfdump.c: CTF dumper.
 *
 * Copyright (C) 2008 David S. Miller <davem@davemloft.net>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>
#include <limits.h>
#include <libgen.h>
#include <zlib.h>

#include <gelf.h>

#include "libctf.h"
#include "ctf.h"
#include "dutil.h"
#include "dwarves.h"

/*
 * FIXME: We should just get the table from the CTF ELF section
 * and use it directly
 */
extern struct strings *strings;

static void *tag__alloc(const size_t size)
{
	struct tag *self = zalloc(size);

	if (self != NULL)
		self->top_level = 1;

	return self;
}

static void oom(const char *msg)
{
	fprintf(stderr, "libclasses: out of memory(%s)\n", msg);
	exit(EXIT_FAILURE);
}

#if 0
static int ctf_ignores_elf_symbol(GElf_Sym *sym, char *name, int type)
{
	if (type == STT_OBJECT &&
	    sym->st_shndx == SHN_ABS &&
	    sym->st_value == 0)
		return 1;
	if (sym->st_name == 0)
		return 1;
	if (sym->st_shndx == SHN_UNDEF)
		return 1;
	if (!strcmp(name, "_START_") || !strcmp(name, "_END_"))
		return 1;
	return 0;
}
#endif

static char *ctf_format_flt_attrs(uint32_t eval, char *buf)
{
	uint32_t attrs = CTF_TYPE_FP_ATTRS(eval);

	buf[0] = '\0';

	if (attrs < CTF_TYPE_FP_SINGLE ||
	    attrs > CTF_TYPE_FP_MAX)
		buf += sprintf(buf, "0x%02x ", attrs);
	else {
		switch (attrs) {
		case CTF_TYPE_FP_SINGLE:
			buf += sprintf(buf, "single ");
			break;
		case CTF_TYPE_FP_DOUBLE:
			buf += sprintf(buf, "double ");
			break;
		case CTF_TYPE_FP_CMPLX:
			buf += sprintf(buf, "complex ");
			break;
		case CTF_TYPE_FP_CMPLX_DBL:
			buf += sprintf(buf, "complex double ");
			break;
		case CTF_TYPE_FP_CMPLX_LDBL:
			buf += sprintf(buf, "complex long double ");
			break;
		case CTF_TYPE_FP_LDBL:
			buf += sprintf(buf, "long double ");
			break;
		case CTF_TYPE_FP_INTVL:
			buf += sprintf(buf, "interval ");
			break;
		case CTF_TYPE_FP_INTVL_DBL:
			buf += sprintf(buf, "interval double ");
			break;
		case CTF_TYPE_FP_INTVL_LDBL:
			buf += sprintf(buf, "interval long double ");
			break;
		case CTF_TYPE_FP_IMGRY:
			buf += sprintf(buf, "imaginary ");
			break;
		case CTF_TYPE_FP_IMGRY_DBL:
			buf += sprintf(buf, "imaginary double ");
			break;
		case CTF_TYPE_FP_IMGRY_LDBL:
			buf += sprintf(buf, "imaginary long double ");
			break;
		}
	}

	return buf;
}

#if 0
static int dump_one_func(struct ctf_state *sp, const char *sym_name,
			 int sym_index, int call_index, void *data)
{
	uint16_t **func_pp = data;
	uint16_t val = ctf__get16(sp->ctf, *func_pp);
	uint16_t type = CTF_GET_KIND(val);
	uint16_t vlen = CTF_GET_VLEN(val);
	uint16_t i;

	(*func_pp)++;

	if (type == CTF_TYPE_KIND_UNKN && vlen == 0)
		return 0;

	if (type != CTF_TYPE_KIND_FUNC) {
		fprintf(stderr, "Expected function type, got %u\n", type);
		exit(2);
	}

	fprintf(stdout, "  [%6d] %-36s %8d\n",
		call_index, sym_name, sym_index);
	fprintf(stdout, "           0x%04x   (",
		ctf__get16(sp->ctf, *func_pp));

	(*func_pp)++;
	for (i = 0; i < vlen; i++) {
		if (i >= 1)
			fprintf(stdout, ", ");

		fprintf(stdout, "0x%04x", ctf__get16(sp->ctf, *func_pp));
		(*func_pp)++;
	}
	fprintf(stdout, ")\n");

	return 0;
}

static void dump_funcs(struct ctf_state *sp)
{
	struct ctf_header *hp = ctf__get_buffer(sp->ctf);
	struct elf_sym_iter_state estate;
	uint16_t *func_ptr;

	fprintf(stdout, "CTF Functions:\n");
	fprintf(stdout,
		"  [  Nr  ] "
		"SymName                              "
		"SymIndex\n"
		"           Returns  "
		"Args\n");

	memset(&estate, 0, sizeof(estate));
	func_ptr = ctf__get_buffer(sp->ctf) + sizeof(*hp) +
		ctf__get32(sp->ctf, &hp->ctf_func_off);
	estate.data = &func_ptr;
	estate.func = dump_one_func;
	estate.st_type = STT_FUNC;
	estate.limit = INT_MAX;

	elf_symbol_iterate(sp, &estate);

	fprintf(stdout, "\n");
}
#endif

static struct base_type *base_type__new(const char *name, size_t size)
{
        struct base_type *self = tag__alloc(sizeof(*self));

	if (self != NULL) {
		self->name = strings__add(strings, name);
		self->bit_size = size;
	}
	return self;
}

static void type__init(struct type *self, uint16_t tag,
		       const char *name, size_t size)
{
	INIT_LIST_HEAD(&self->node);
	INIT_LIST_HEAD(&self->namespace.tags);
	self->size = size;
	self->namespace.tag.tag = tag;
	self->namespace.name = strings__add(strings, name);
}

static struct type *type__new(uint16_t tag, const char *name, size_t size)
{
        struct type *self = tag__alloc(sizeof(*self));

	if (self != NULL)
		type__init(self, tag, name, size);

	return self;
}

static struct class *class__new(const char *name, size_t size)
{
	struct class *self = tag__alloc(sizeof(*self));

	if (self != NULL) {
		type__init(&self->type, DW_TAG_structure_type, name, size);
		INIT_LIST_HEAD(&self->vtable);
	}

	return self;
}

static int create_new_base_type(struct ctf *self, void *ptr,
				struct ctf_full_type *tp, long id)
{
	uint32_t *enc = ptr, name_idx;
	char name[64], *buf = name;
	uint32_t eval = ctf__get32(self, enc);
	uint32_t attrs = CTF_TYPE_INT_ATTRS(eval);
	struct base_type *base;

	if (attrs & CTF_TYPE_INT_SIGNED)
		buf += sprintf(buf, "signed ");
	if (attrs & CTF_TYPE_INT_BOOL)
		buf += sprintf(buf, "bool ");
	if (attrs & CTF_TYPE_INT_VARARGS)
		buf += sprintf(buf, "varargs ");

	name_idx = ctf__get32(self, &tp->base.ctf_name);
	buf += sprintf(buf, "%s", ctf__string(self, name_idx));
	base = base_type__new(name, CTF_TYPE_INT_BITS(eval));
	if (base == NULL)
		oom("base_type__new");

	base->tag.tag = DW_TAG_base_type;
	cu__add_tag(self->priv, &base->tag, &id);

	return sizeof(*enc);
}

static int create_new_base_type_float(struct ctf *self, void *ptr,
				      struct ctf_full_type *tp,
				      long id)
{
	uint32_t *enc = ptr, eval;
	char name[64];
	struct base_type *base;

	eval = ctf__get32(self, enc);
	sprintf(ctf_format_flt_attrs(eval, name), "%s",
		ctf__string32(self, &tp->base.ctf_name));

	base = base_type__new(name, CTF_TYPE_FP_BITS(eval));
	if (base == NULL)
		oom("base_type__new");

	base->tag.tag = DW_TAG_base_type;
	cu__add_tag(self->priv, &base->tag, &id);

	return sizeof(*enc);
}

static int create_new_array(struct ctf *self, void *ptr, long id)
{
	struct ctf_array *ap = ptr;
	struct array_type *array = tag__alloc(sizeof(*array));

	if (array == NULL)
		oom("array_type");

	/* FIXME: where to get the number of dimensions?
	 * it it flattened? */
	array->dimensions = 1;
	array->nr_entries = malloc(sizeof(uint32_t));

	if (array->nr_entries == NULL)
		oom("array_type->nr_entries");

	array->nr_entries[0] = ctf__get32(self, &ap->ctf_array_nelems);
	array->tag.tag = DW_TAG_array_type;
	array->tag.type = ctf__get16(self, &ap->ctf_array_type);

	cu__add_tag(self->priv, &array->tag, &id);

	return sizeof(*ap);
}

static int create_new_subroutine_type(struct ctf *self, void *ptr,
				      int vlen, struct ctf_full_type *tp,
				      long id)
{
	uint16_t *args = ptr;
	uint16_t i;
	const char *name = ctf__string32(self, &tp->base.ctf_name);
	unsigned int type = ctf__get16(self, &tp->base.ctf_type);
	struct function *function = tag__alloc(sizeof(*function));

	if (function == NULL)
		oom("function__new");

	function->name = strings__add(strings, name);
	INIT_LIST_HEAD(&function->vtable_node);
	INIT_LIST_HEAD(&function->tool_node);
	INIT_LIST_HEAD(&function->proto.parms);
	function->proto.tag.tag = DW_TAG_subroutine_type;
	function->proto.tag.type = type;
	INIT_LIST_HEAD(&function->lexblock.tags);

	for (i = 0; i < vlen; i++) {
		uint16_t type = ctf__get16(self, &args[i]);

		if (type == 0)
			function->proto.unspec_parms = 1;
		else {
			struct parameter *p = tag__alloc(sizeof(*p));

			p->tag.tag  = DW_TAG_formal_parameter;
			p->tag.type = ctf__get16(self, &args[i]);
			ftype__add_parameter(&function->proto, p);
		}
	}

	vlen *= sizeof(*args);

	/* Round up to next multiple of 4 to maintain
	 * 32-bit alignment.
	 */
	if (vlen & 0x2)
		vlen += 0x2;

	cu__add_tag(self->priv, &function->proto.tag, &id);

	return vlen;
}

static unsigned long create_full_members(struct ctf *self, void *ptr,
					 int vlen, struct type *class)
{
	struct ctf_full_member *mp = ptr;
	int i;

	for (i = 0; i < vlen; i++) {
		struct class_member *member = zalloc(sizeof(*member));

		if (member == NULL)
			oom("class_member");

		member->tag.tag = DW_TAG_member;
		member->tag.type = ctf__get16(self, &mp[i].ctf_member_type);
		member->name = strings__add(strings,
					    ctf__string32(self,
							  &mp[i].ctf_member_name));
		member->bit_offset = (ctf__get32(self, &mp[i].ctf_member_offset_high) << 16) |
				      ctf__get32(self, &mp[i].ctf_member_offset_low);
		/* sizes and offsets will be corrected at class__fixup_ctf_bitfields */
		type__add_member(class, member);
	}

	return sizeof(*mp);
}

static unsigned long create_short_members(struct ctf *self, void *ptr,
					  int vlen, struct type *class)
{
	struct ctf_short_member *mp = ptr;
	int i;

	for (i = 0; i < vlen; i++) {
		struct class_member *member = zalloc(sizeof(*member));

		if (member == NULL)
			oom("class_member");

		member->tag.tag = DW_TAG_member;
		member->tag.type = ctf__get16(self, &mp[i].ctf_member_type);
		member->name = strings__add(strings,
					    ctf__string32(self,
							  &mp[i].ctf_member_name));
		member->bit_offset = ctf__get16(self, &mp[i].ctf_member_offset);
		/* sizes and offsets will be corrected at class__fixup_ctf_bitfields */

		type__add_member(class, member);
	}

	return sizeof(*mp);
}

static int create_new_class(struct ctf *self, void *ptr,
			    int vlen, struct ctf_full_type *tp,
			    uint64_t size, long id)
{
	unsigned long member_size;
	const char *name = ctf__string32(self, &tp->base.ctf_name);
	struct class *class = class__new(name, size);

	if (size >= CTF_SHORT_MEMBER_LIMIT) {
		member_size = create_full_members(self, ptr, vlen, &class->type);
	} else {
		member_size = create_short_members(self, ptr, vlen, &class->type);
	}

	cu__add_tag(self->priv, &class->type.namespace.tag, &id);

	return (vlen * member_size);
}

static int create_new_union(struct ctf *self, void *ptr,
			    int vlen, struct ctf_full_type *tp,
			    uint64_t size, long id)
{
	unsigned long member_size;
	const char *name = ctf__string32(self, &tp->base.ctf_name);
	struct type *un = type__new(DW_TAG_union_type, name, size);

	if (size >= CTF_SHORT_MEMBER_LIMIT) {
		member_size = create_full_members(self, ptr, vlen, un);
	} else {
		member_size = create_short_members(self, ptr, vlen, un);
	}

	cu__add_tag(self->priv, &un->namespace.tag, &id);

	return (vlen * member_size);
}

static struct enumerator *enumerator__new(const char *name,
					  uint32_t value)
{
	struct enumerator *self = tag__alloc(sizeof(*self));

	if (self != NULL) {
		self->name = strings__add(strings, name);
		self->value = value;
		self->tag.tag = DW_TAG_enumerator;
	}

	return self;
}

static int create_new_enumeration(struct ctf *self, void *ptr,
				  int vlen, struct ctf_full_type *tp,
				  uint16_t size, long id)
{
	struct ctf_enum *ep = ptr;
	uint16_t i;
	struct type *enumeration = type__new(DW_TAG_enumeration_type,
					     ctf__string32(self,
							   &tp->base.ctf_name),
					     size ?: (sizeof(int) * 8));

	if (enumeration == NULL)
		oom("enumeration");

	for (i = 0; i < vlen; i++) {
		char *name = ctf__string32(self, &ep[i].ctf_enum_name);
		uint32_t value = ctf__get32(self, &ep[i].ctf_enum_val);
		struct enumerator *enumerator = enumerator__new(name, value);

		if (enumerator == NULL)
			oom("enumerator__new");

		enumeration__add(enumeration, enumerator);
	}

	cu__add_tag(self->priv, &enumeration->namespace.tag, &id);

	return (vlen * sizeof(*ep));
}

static int create_new_forward_decl(struct ctf *self, struct ctf_full_type *tp,
				   uint64_t size, long id)
{
	char *name = ctf__string32(self, &tp->base.ctf_name);
	struct class *fwd = class__new(name, size);

	if (fwd == NULL)
		oom("class foward decl");
	fwd->type.declaration = 1;
	cu__add_tag(self->priv, &fwd->type.namespace.tag, &id);
	return 0;
}

static int create_new_typedef(struct ctf *self, struct ctf_full_type *tp,
			      uint64_t size, long id)
{
	const char *name = ctf__string32(self, &tp->base.ctf_name);
	unsigned int type_id = ctf__get16(self, &tp->base.ctf_type);
	struct type *type = type__new(DW_TAG_typedef, name, size);

	if (type == NULL)
		oom("type__new");

	type->namespace.tag.type = type_id;
	cu__add_tag(self->priv, &type->namespace.tag, &id);

	return 0;
}

static int create_new_tag(struct ctf *self, int type,
			  struct ctf_full_type *tp, long id)
{
	unsigned int type_id = ctf__get16(self, &tp->base.ctf_type);
	struct tag *tag = zalloc(sizeof(*tag));

	if (tag == NULL)
		oom("tag__new");

	switch (type) {
	case CTF_TYPE_KIND_CONST:	tag->tag = DW_TAG_const_type;	 break;
	case CTF_TYPE_KIND_PTR:		tag->tag = DW_TAG_pointer_type;  break;
	case CTF_TYPE_KIND_RESTRICT:	tag->tag = DW_TAG_restrict_type; break;
	case CTF_TYPE_KIND_VOLATILE:	tag->tag = DW_TAG_volatile_type; break;
	default:
		printf("%s: FOO %d\n\n", __func__, type);
		return 0;
	}

	tag->type = type_id;
	cu__add_tag(self->priv, tag, &id);

	return 0;
}

static void ctf__load_types(struct ctf *self)
{
	void *ctf_buffer = ctf__get_buffer(self);
	struct ctf_header *hp = ctf_buffer;
	void *ctf_contents = ctf_buffer + sizeof(*hp),
	     *type_section = (ctf_contents +
			      ctf__get32(self, &hp->ctf_type_off)),
	     *strings_section = (ctf_contents +
				 ctf__get32(self, &hp->ctf_str_off));
	struct ctf_full_type *type_ptr = type_section,
			     *end = strings_section;
	unsigned int type_index = 0x0001;

	if (hp->ctf_parent_name || hp->ctf_parent_label)
		type_index += 0x8000;

	while (type_ptr < end) {
		uint16_t val, type, vlen, base_size;
		uint64_t size;
		void *ptr;

		val = ctf__get16(self, &type_ptr->base.ctf_info);
		type = CTF_GET_KIND(val);
		vlen = CTF_GET_VLEN(val);

		base_size = ctf__get16(self, &type_ptr->base.ctf_size);
		ptr = type_ptr;
		if (base_size == 0xffff) {
			size = ctf__get32(self, &type_ptr->ctf_size_high);
			size <<= 32;
			size |= ctf__get32(self, &type_ptr->ctf_size_low);
			ptr += sizeof(struct ctf_full_type);
		} else {
			size = base_size;
			ptr += sizeof(struct ctf_short_type);
		}

		if (type == CTF_TYPE_KIND_INT) {
			vlen = create_new_base_type(self, ptr, type_ptr, type_index);
		} else if (type == CTF_TYPE_KIND_FLT) {
			vlen = create_new_base_type_float(self, ptr, type_ptr, type_index);
		} else if (type == CTF_TYPE_KIND_ARR) {
			vlen = create_new_array(self, ptr, type_index);
		} else if (type == CTF_TYPE_KIND_FUNC) {
			vlen = create_new_subroutine_type(self, ptr, vlen, type_ptr, type_index);
		} else if (type == CTF_TYPE_KIND_STR) {
			vlen = create_new_class(self, ptr,
						vlen, type_ptr, size, type_index);
		} else if (type == CTF_TYPE_KIND_UNION) {
			vlen = create_new_union(self, ptr,
					        vlen, type_ptr, size, type_index);
		} else if (type == CTF_TYPE_KIND_ENUM) {
			vlen = create_new_enumeration(self, ptr, vlen, type_ptr,
						      size, type_index);
		} else if (type == CTF_TYPE_KIND_FWD) {
			vlen = create_new_forward_decl(self, type_ptr, size, type_index);
		} else if (type == CTF_TYPE_KIND_TYPDEF) {
			vlen = create_new_typedef(self, type_ptr, size, type_index);
		} else if (type == CTF_TYPE_KIND_VOLATILE ||
			   type == CTF_TYPE_KIND_PTR ||
			   type == CTF_TYPE_KIND_CONST ||
			   type == CTF_TYPE_KIND_RESTRICT) {
			vlen = create_new_tag(self, type, type_ptr, type_index);
		} else if (type == CTF_TYPE_KIND_UNKN) {
			cu__table_nullify_type_entry(self->priv, type_index);
			fprintf(stderr,
				"CTF: idx: %d, off: %lu, root: %s Unknown\n",
				type_index, ((void *)type_ptr) - type_section,
				CTF_ISROOT(val) ? "yes" : "no");
			vlen = 0;
		} else {
			abort();
		}

		type_ptr = ptr + vlen;
		type_index++;
	}
}

static void ctf__load_sections(struct ctf *self)
{
	//dump_funcs(self);
	ctf__load_types(self);
}

static int class__fixup_ctf_bitfields(struct tag *self, struct cu *cu)
{
	struct class_member *pos;
	struct type *type_self = tag__type(self);

	type__for_each_data_member(type_self, pos) {
		struct tag *type = tag__follow_typedef(&pos->tag, cu);

		pos->bitfield_offset = 0;
		pos->bitfield_size = 0;
		pos->byte_offset = pos->bit_offset / 8;

		uint16_t type_bit_size;
		size_t integral_bit_size;

		switch (type->tag) {
		case DW_TAG_enumeration_type:
			type_bit_size = tag__type(type)->size;
			/* Best we can do to check if this is a packed enum */
			if (is_power_of_2(type_bit_size))
				integral_bit_size = roundup(type_bit_size, 8);
			else
				integral_bit_size = sizeof(int) * 8;
			break;
		case DW_TAG_base_type: {
			struct base_type *bt = tag__base_type(type);
			type_bit_size = bt->bit_size;
			integral_bit_size = base_type__name_to_size(bt, cu);
		}
			break;
		default:
			pos->byte_size = tag__size(type, cu);
			pos->bit_size = pos->byte_size * 8;
			continue;
		}

		/*
		 * XXX: integral_bit_size can be zero if base_type__name_to_size doesn't
		 * know about the base_type name, so one has to add there when
		 * such base_type isn't found. pahole will put zero on the
		 * struct output so it should be easy to spot the name when
		 * such unlikely thing happens.
		 */
		pos->byte_size = integral_bit_size / 8;

		if (integral_bit_size == 0 || type_bit_size == integral_bit_size) {
			if (integral_bit_size == 0)
				fprintf(stderr, "boo!\n");
			pos->bit_size = integral_bit_size;
			continue;
		}

		pos->bitfield_offset = pos->bit_offset % integral_bit_size;
		pos->bitfield_size = type_bit_size;
		pos->bit_size = type_bit_size;
		pos->byte_offset = (((pos->bit_offset / integral_bit_size) *
				     integral_bit_size) / 8);
	}

	return 0;
}

static int cu__fixup_ctf_bitfields(struct cu *self)
{
	int err = 0;
	struct tag *pos;

	list_for_each_entry(pos, &self->tags, node)
		if (tag__is_struct(pos) || tag__is_union(pos)) {
			err = class__fixup_ctf_bitfields(pos, self);
			if (err)
				break;
		}

	return err;
}

int ctf__load_file(struct cus *self, struct conf_load *conf,
		   const char *filename)
{
	int err;
	struct ctf *state = ctf__new(filename);

	if (state == NULL)
		return -1;

	struct cu *cu = cu__new(filename, state->wordsize, NULL, 0, filename);
	if (cu == NULL)
		return -1;

	state->priv = cu;
	if (ctf__load(state) != 0)
		return -1;

	ctf__load_sections(state);

	ctf__delete(state);

	base_type_name_to_size_table__init();
	err = cu__fixup_ctf_bitfields(cu);
	/*
	 * The app stole this cu, possibly deleting it,
	 * so forget about it
	 */
	if (conf && conf->steal && conf->steal(cu, conf))
		return 0;

	cus__add(self, cu);
	return err;
}
