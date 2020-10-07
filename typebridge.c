/*
 * Typebridge -- a tool to convert a generated Scade-model's type-map to a dataset description for TRDP
 *
 * File:      typebridge.c
 * Author:    Thorsten Schulz <thorsten.schulz@ui-rostock.de>
 * Copyright: (c) 2018-2020 Universität Rostock
 * SPDX-Identifier: Apache-2.0
 *
 * build-dep: libmxml-dev
 */

/*
 * some more clarifications:
 *
 * I will only scan for *complex* inputs and outputs. Basic / predefined types are
 * typically only local and should not inflict TRDP data-sets.
 * Otherwise, such an I/O must be wrapped in a structure or an array.
 *
 * One more thing, arrays can only be one-dimensional. Array of Array, a[m][n]
 * cannot be mapped. You can have an array of structs containing an array. But
 * in C this is not the same thing, even though in RAM it may be.
 *
 * Really, try to use tau_xmarshall and tau_xsession. It will make life easier.
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <mxml.h>

#define SCADE_MAP_DEFAULT "mapping.xml"

char *xformSep(const char *in, const char *sepIn, const char *sepOut) {
	char *out = NULL;
	if (!in) return NULL; /* nothing to do */
	size_t sil = sepIn  ? strlen(sepIn)  : 0;
	size_t sol = sepOut ? strlen(sepOut) : 0;
	if (sil && (sil < sol)) return NULL; /* case currently not implemented */
	out = strdup(in);
	if (!out || !sil) return out;
	char *tik = strstr( in, sepIn);
	char *tok = tik ? out+(tik-in) : NULL;

	while (tik) {
		strcpy(tok, sepOut);
		tok += sol;
		tik += sil;
		strcpy(tok, tik);
		char *ntik = strstr(tik, sepIn);
		if (ntik) tok += ntik-tik;
		tik = ntik;
	}

	return out;
}

const char *getRootName(mxml_node_t *doc) {
	mxml_node_t *mapnode, *catnode, *ornode;

	mapnode = mxmlFindElement(doc, doc, "mapping", NULL, NULL, MXML_DESCEND_FIRST);
	catnode = mxmlFindElement(mapnode, mapnode, "config", NULL, NULL, MXML_DESCEND_FIRST);
	ornode  = mxmlFindElement(catnode, catnode, "option", "name", "root", MXML_DESCEND);
	const char *root_name = mxmlElementGetAttr(ornode, "value");

	if (root_name && strnlen(root_name, 0x1000) < 0x1000) {
		fprintf(stderr, "[ OK ] Identified root name: %s\n",root_name);
		return root_name;
	} else {
		return NULL;
	}
}

mxml_node_t *findOperator(mxml_node_t *doc, const char *root_name) {
	mxml_node_t *root = NULL;
	mxml_node_t *mapnode, *catnode;

	mapnode = mxmlFindElement(doc, doc, "mapping", NULL, NULL, MXML_DESCEND_FIRST);

	if (root_name) {
		catnode = mxmlFindElement(mapnode, mapnode, "model", NULL, NULL, MXML_DESCEND_FIRST);
		mxml_node_t *pnode = catnode;

		char *path = strdup(root_name);
		if (!path) return NULL;
		char * tok = path;
		size_t spl = 2; /* = strlen("::"); */
		char *ntok = strstr(path, "::");

		while (ntok) {
			*ntok = '\0';
			pnode = mxmlFindElement(pnode, pnode, "package", "name", tok, MXML_DESCEND_FIRST);
			tok = ntok+spl;
			ntok = strstr(tok, "::");
		}
		root = mxmlFindElement(pnode, pnode, "operator", "name", tok, MXML_DESCEND);

		if (!root)
			fprintf(stderr, "[FAIL] Operator \"%s\" not found.\n", root_name);
		else if (mxmlFindElement(root, pnode, "operator", "name", tok, MXML_DESCEND)) {
			fprintf(stderr, "[FAIL] Encountered multiple matching operators for \"%s\". Add package path.\n", root_name);
			root = NULL;
		} else {
			fprintf(stderr, "[ OK ] \"%s", mxmlElementGetAttr(root, "name"));
			pnode = mxmlGetParent(root);
			while (pnode != catnode) {
				fprintf(stderr, "<<%s", mxmlElementGetAttr(pnode, "name"));
				pnode = mxmlGetParent(pnode);
			}
			fprintf(stderr, "\"\n");
		}

		if (path) free(path);
	} else {
		fprintf(stderr, "[FAIL] Operator not defined.\n");
	}
	return root;
}

/*
 * <array id="%id" baseType="%id" size="n" />                                <-- appear only directly below <model>
 * <struct id="%id"> <field id="%id" name="fieldname" type="%id"/> </struct> <-- appear only directly below <model>
 * <predefType id="%id" name="Scade-Type-Name" />                            <-- mapping to base types
 * <type id="%id" name="Scade_Name" type="%id" />                            <-- naming of exiting type
 */
#define USE_TRDP_NUMTYPES 0  /* whether to use "10" or "UINT32" */
#define KCG_SIZE_MAPS_TO 6   /* put the matching target id of the TRDP type here (6 == INT32) */
#define DATASETID_LEN 12     /* incl. term. \0 */
#define SCADE_MIDs 0x4000

typedef struct type_entry {
	char dataSetId[DATASETID_LEN]; /* the data-set-id in the type-field as string */
	int dsid;              /* integer representation of above */
	/*int mid; <- index */ /* model id within mapping.xml */
	int ref_cnt;           /* >0 if the root-operator makes use of the type and it should be included */
	int reference_of_mid;  /* this entry is a reference of that entry, -1 for a base type */
	int size;              /* array size */
	char *name;            /* data-sets and their elements optional have descriptive names, length-limit ds:[1..30],
	                          element unbound */
} type_entry_t;

type_entry_t mid_map[SCADE_MIDs];
#define count_of(a) (sizeof(a)/sizeof(a[0]))

const char *ScadeBaseTypes[] = { "",
		"bool", "char", "wchar",
		"int8", "int16", "int32", "int64",
		"uint8", "uint16", "uint32", "uint64",
		"float32", "float64",
		"timedate32", "timedate48", "timedate64",
		"size"
};

#if USE_TRDP_NUMTYPES
const char ScadeTypeIdx2TRDP[DATASETID_LEN][] = {
		"", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"
};
#else
const char *ScadeTypeIdx2TRDP[] = { "",
		/* 1*/ "BOOL8", "CHAR8", "UTF16",
		/* 4*/ "INT8", "INT16", "INT32", "INT64",
		/* 8*/ "UINT8", "UINT16", "UINT32", "UINT64",
		/*12*/ "REAL32", "REAL64",
		/*14*/ "TIMEDATE32", "TIMEDATE48", "TIMEDATE64"
};
#endif
/*
		"BITSET8 01 BOOL8 01 ANTIVALENT8 02 CHAR8 02 UTF8 03 UTF16 04 INT8 05 INT16 06 INT32 07 INT64 "
		"08 UINT8 09 UINT16 10 UINT32 11 UINT64 12 REAL32 13 REAL64 14 TIMEDATE32 15 TIMEDATE48 16 TIMEDATE64"
*/

bool addType(long mid, const char *name, long type, long dsid, long cnt) {
	if (mid > 0 && mid < SCADE_MIDs) {
		if (mid_map[mid].dsid <= 0) {
			if (dsid <= 0 || dsid >= count_of(ScadeTypeIdx2TRDP)) {
				mid_map[mid].dsid = 1000+mid;
				snprintf(mid_map[mid].dataSetId, DATASETID_LEN, "%d", mid_map[mid].dsid);
				mid_map[mid].reference_of_mid = type;
				mid_map[mid].size = cnt;
				mid_map[mid].name = name ? strdup(name) : NULL;
			} else {
				mid_map[mid].dsid = dsid;
				strncpy(mid_map[mid].dataSetId, ScadeTypeIdx2TRDP[dsid], DATASETID_LEN);
				mid_map[mid].reference_of_mid = -1;
				mid_map[mid].size = 0;
				mid_map[mid].name = NULL;
			}
			mid_map[mid].ref_cnt = 0;
			return true;
		} else {
			fprintf(stderr, "[CRIT] Scade model id=%ld not defined again.\n", mid);
		}
	} else {
		fprintf(stderr, "[ERR ] Scade model id=%ld off scope ('%s' type=%ld dsid=%ld cnt=%ld).\n", mid, name, type, dsid, cnt);
	}

	return false;
}

/* Stitch s1 and s2 together with separator sep inbetween. If one string is NULL only the other is duplicated. The 
 * returned string length will be at most maxlen characters and trailing zero is added. As a special behaviour, if 
 * strings are stitched, and the result is longer, they will be cut from the front.
 */
char *strndup2(const char *s1, const char *s2, char sep, size_t maxlen) {
	if ((!s1 && !s2) || !maxlen) return NULL;
	if (( s1 && !s2)) return strndup(s1, (size_t)maxlen);
	if ((!s1 &&  s2)) return strndup(s2, (size_t)maxlen);
	size_t l1 = strlen(s1);
	size_t l  = l1+1+strlen(s2);
	char *s = malloc(l+1);
	if (s) {
		char *dup = strcpy(s, s1) + l1;
		if (sep) *dup++ = sep;
		strcpy(dup, s2);
		/* this is much simpler, than trying to cut each part individually. */
		if (l > (size_t)maxlen) {
			memmove(s, s + (l - maxlen), maxlen+1);
		}
	}
	return s;
}

bool propagateName(long mid, const char *name, const char *pkgname) {
	if (mid > 0 && mid < SCADE_MIDs) {
		if (mid_map[mid].reference_of_mid < 0 && mid_map[mid].size > 0) { /* only worry about structs */
			if (mid_map[mid].dsid > 0) {
				if (!mid_map[mid].name) {
					mid_map[mid].name = strndup2(pkgname, name, '_', 30);
					return true;
				} else {
					fprintf(stderr, "[CRIT] Model id %ld = \"%s\" should be renamed \"%s\".\n", 
								mid, mid_map[mid].name, name);
				}
			} else {
				fprintf(stderr, "[CRIT] Model id %ld not defined.\n", mid);
			}
		}
	}
	return false;
}

bool attrToInt(mxml_node_t *node, const char *a, long min, long max, long *v) {
	if (!node || !a || !v) return false;
	const char *ch = mxmlElementGetAttr(node, a);
	char *ch_e;
	if (ch && *ch) {
		long val = strtol(ch, &ch_e, 10);
		if (!*ch_e && min <= val && val <= max) {
			*v = val;
			return true;
		} else {
			fprintf(stderr, "[WARN] %s.%s = \"%s\" is invalid.\n", mxmlGetElement(node), a, ch);
		}

	} else {
		fprintf(stderr, "[WARN] %s.%s not set.\n", mxmlGetElement(node), a);
	}

	return false;
}

int require(long mid) {
	if (mid > 0 && mid < SCADE_MIDs) {
		int sub = mid_map[mid].size;
		mid_map[mid].ref_cnt++;
		if (mid_map[mid].reference_of_mid > 0) {
			if (mid_map[mid].reference_of_mid == mid)
				fprintf(stderr, "[ERR ] mid=%ld is self-referencing. bug.\n", mid);
			else
				sub += require(mid_map[mid].reference_of_mid);
		} else {
			/* if mid has no ref, but a size, then it is a struct and we need to include all elements */
			for (long i=1; i<=mid_map[mid].size; i++) sub += require(mid+i);
		}
		return (sub>0);
	} else {
		fprintf(stderr, "[ERR ] mid=%ld is out of scope. bug.\n", mid);
		return 0;
	}
}

int scanTypes_recurse_pkg(mxml_node_t *parent, const char *parentname) {
	int typerefs = 0;
	mxml_node_t *pkgnode   = mxmlFindElement(parent, parent, "package", NULL, NULL, MXML_DESCEND_FIRST);
	while (pkgnode) {
		char *pkgname = strndup2(parentname, mxmlElementGetAttr(pkgnode, "name"), '_', -1);
		mxml_node_t *tnode  = mxmlFindElement(pkgnode, pkgnode, "type", NULL, NULL, MXML_DESCEND_FIRST);
		while (tnode) {
			long mid, emid;
			const char *name = mxmlElementGetAttr(tnode, "name");

			typerefs +=
				attrToInt(tnode,   "id", 1, SCADE_MIDs-1, &mid )
				&& attrToInt(tnode, "type", 1, SCADE_MIDs-1, &emid)
				&& addType(mid, name, /*type=*/emid, /*dsid*/-1, 0)
				&& propagateName(emid, name, pkgname);

			tnode  = mxmlFindElement(tnode, pkgnode, "type", NULL, NULL, MXML_NO_DESCEND);
		}
		typerefs += scanTypes_recurse_pkg(pkgnode, pkgname);
			/* don't let mxml do the descending */
		pkgnode  = mxmlFindElement(pkgnode, parent, "package", NULL, NULL, MXML_NO_DESCEND);
		free(pkgname);
	}
	return typerefs;
}

void scanTypes(mxml_node_t *doc) {
	long arrays=0, structs=0, typerefs=0;

	mxml_node_t *mapnode = mxmlFindElement(doc, doc, "mapping", NULL, NULL, MXML_DESCEND_FIRST);
	mxml_node_t *catnode = mxmlFindElement(mapnode, mapnode, "model", NULL, NULL, MXML_DESCEND_FIRST);

	mxml_node_t *tnode   = mxmlFindElement(catnode, catnode, "predefType", NULL, NULL, MXML_DESCEND_FIRST);
	while (tnode) {
		const char *sctype_name = mxmlElementGetAttr(tnode, "name");
		long mid;
		if (attrToInt(tnode, "id", 1, SCADE_MIDs-1, &mid)) {
			long dsid = count_of(ScadeBaseTypes);
			while (--dsid >= 0 && strcasecmp(sctype_name, ScadeBaseTypes[dsid]));

			if (dsid > 0) {
				if (dsid >= count_of(ScadeTypeIdx2TRDP)) dsid = KCG_SIZE_MAPS_TO;
				addType(mid, /*scname=*/NULL, /*type=*/-1, dsid, /* count */0);
			} else {
				fprintf(stderr, "[CRIT] Unknown Scade predef type definition (\"%s\").\n", sctype_name);
			}
		}
		tnode  = mxmlFindElement(tnode, catnode, "predefType", NULL, NULL, MXML_NO_DESCEND);
	}

	tnode = mxmlFindElement(catnode, catnode, "array", NULL, NULL, MXML_DESCEND_FIRST);
	while (tnode) {
		long mid, emid, cnt;

		arrays +=
			attrToInt(tnode,   "id", 1, SCADE_MIDs-1, &mid )
			&& attrToInt(tnode, "baseType", 1, SCADE_MIDs-1, &emid)
			&& attrToInt(tnode, "size", 1,       0xFFFF, &cnt )
			&& addType(mid, /*scname=*/NULL, /*type=*/emid, /*dsid*/-1, cnt);

		tnode  = mxmlFindElement(tnode, catnode, "array", NULL, NULL, MXML_NO_DESCEND);
	}

	tnode   = mxmlFindElement(catnode, catnode, "struct", NULL, NULL, MXML_DESCEND_FIRST);
	while (tnode) {
		long mid;

		if (attrToInt(tnode, "id", 1, SCADE_MIDs-1, &mid)) {
			int fields=0;
			mxml_node_t *fnode  = mxmlFindElement(tnode, tnode, "field", NULL, NULL, MXML_DESCEND_FIRST);
			while (fnode) {
				long fmid, emid;
				const char *name = mxmlElementGetAttr(fnode, "name");

				fields +=
					attrToInt(fnode,   "id", 1, SCADE_MIDs-1, &fmid)
					&& attrToInt(fnode, "type", 1, SCADE_MIDs-1, &emid)
					&& addType(fmid, /*scname=*/name, /*elmid*/emid, /*dsid*/-1, 0);

				fnode  = mxmlFindElement(fnode, tnode, "field", NULL, NULL, MXML_NO_DESCEND);
			}
			structs +=
			addType(mid, /*scname=*/NULL, /*elmid*/-1, /*dsid*/-1, fields);
			/* names for structs can later be inherited from <type> definitions, but must
			 * be limited to 30 characters. They should contain the package path, but that
			 * wont fit. Any ideas?
			 */
		}
		tnode  = mxmlFindElement(tnode, catnode, "struct", NULL, NULL, MXML_NO_DESCEND);
	}

	tnode   = mxmlFindElement(catnode, catnode, "type", NULL, NULL, MXML_DESCEND_FIRST);
	while (tnode) {
		long mid, emid;
		const char *name = mxmlElementGetAttr(tnode, "name");

		typerefs +=
			attrToInt(tnode,   "id", 1, SCADE_MIDs-1, &mid )
			&& attrToInt(tnode, "type", 1, SCADE_MIDs-1, &emid)
			&& addType(mid, name, /*type=*/emid, /*dsid*/-1, 0)
			&& propagateName(emid, name, NULL);

			/* do not freely descend, we need the package names */
		tnode  = mxmlFindElement(tnode, catnode, "type", NULL, NULL, MXML_NO_DESCEND);
	}

	typerefs += scanTypes_recurse_pkg(catnode, NULL);
	
	fprintf(stderr, "[ OK ] Found %ld arrays, %ld structs, %ld type instantiations.\n", arrays, structs, typerefs);
}

mxml_node_t *getInputsForOperator(mxml_node_t *opr) {
	mxml_node_t *list = mxmlNewElement(NULL, "inputs");
	mxml_node_t *n, *i = opr;
	int cnt = 0, req = 0;
	while ((i = mxmlFindElement(i, opr, "input", NULL, NULL, MXML_DESCEND_FIRST))) {
		long mid;
		if (attrToInt(i, "type", 0, SCADE_MIDs-1, &mid)) {
			req += require(mid);
			cnt++;
		}
		n = mxmlNewElement(list, "input");
		mxmlElementSetAttr(n, "name", mxmlElementGetAttr(i, "name"));
		mxmlElementSetAttr(n, "type", mxmlElementGetAttr(i, "type"));
	};
	if (cnt) fprintf(stderr, "[%s] has %2d DS-inputs out of %2d\n", req>0?"INFO":"WARN", req, cnt);
	return list;
}

mxml_node_t *getOutputsForOperator(mxml_node_t *opr) {
	mxml_node_t *list = mxmlNewElement(NULL, "inputs");
	mxml_node_t *n, *i = opr;
	int cnt = 0, req = 0;
	while ((i = mxmlFindElement(i, opr, "output", NULL, NULL, MXML_DESCEND_FIRST))) {
		long mid;
		if (attrToInt(i, "type", 0, SCADE_MIDs-1, &mid)) {
			req += require(mid);
			cnt++;
		}
		n = mxmlNewElement(list, "output");
		mxmlElementSetAttr(n, "name", mxmlElementGetAttr(i, "name"));
		mxmlElementSetAttr(n, "type", mxmlElementGetAttr(i, "type"));
	};
	if (cnt) fprintf(stderr, "[%s] has %2d DS-outputs out of %2d\n", req>0?"INFO":"WARN", req, cnt);
	return list;
}

mxml_node_t *resolveTRDPDataSets(bool requiredOnly) {
	mxml_node_t *list = mxmlNewElement(NULL, "data-set-list");

	for(long i=0; i<SCADE_MIDs; i++) if (
			(mid_map[i].reference_of_mid <= 0) /*not reference or array */
		&&  (mid_map[i].size > 0) /* fields to follow */
		&&  (mid_map[i].ref_cnt > (-1+requiredOnly)) /* actually required for the operator */) {

			mxml_node_t *dsx = mxmlNewElement(list, "data-set");
			if (mid_map[i].name) mxmlElementSetAttr(dsx, "name", mid_map[i].name);
			mxmlElementSetAttr(dsx, "id", mid_map[i].dataSetId);

			for (long j=i+1; j<=(i+mid_map[i].size); j++) {
				mxml_node_t *dsex = mxmlNewElement(dsx, "element");
				if (mid_map[j].name) mxmlElementSetAttr(dsex, "name", mid_map[j].name);
				long k = j;
				long array = 0;
				while (mid_map[k].reference_of_mid >= 0) {
					k = mid_map[k].reference_of_mid; /* go into sub-type */

					if (mid_map[k].reference_of_mid >= 0 && mid_map[k].size > 0) { /* is it an array */
						if (!array) {
							char buf[16];
							snprintf(buf, sizeof(buf), "%d", mid_map[k].size);
							mxmlElementSetAttr(dsex, "array-size", buf);
							array = k;
						} else {
							fprintf(stderr, "[ERR ] Array of array is not mapable in TRDP. Output may be incomplete"
							                ". Check (DS=%s) %s->%s[%d][%d]\n",
							                mid_map[i].dataSetId, mid_map[i].name,
							                mid_map[j].name, mid_map[array].size, mid_map[k].size);
						}
					}
				}
				mxmlElementSetAttr(dsex, "type", mid_map[k].dataSetId);
			}
			i += mid_map[i].size;
		}
	return list;
}

mxml_node_t *readMapFile(const char *sname) {
	FILE *fp = NULL;
	mxml_node_t *itree= NULL;

	if (sname && sname[0] == '-' && !sname[1]) sname = NULL;
	if (!sname) {
		fp = stdin;
	} else if (strchr(sname, '/') || strstr(sname, ".xml") || strstr(sname, ".XML")) {
		errno = 0;
		fp = fopen(sname, "r");
		if (!fp || errno) {
			fprintf(stderr, "[CRIT] Could not open \"%s\" for reading: ", sname);
			perror(NULL);
			sname = NULL;
		}
	} else {
		fprintf(stderr, "[ERR ] Dubious filename provided for reading: %s\n", sname);
	}

	if (fp) {
		itree = mxmlLoadFile(NULL, fp, MXML_OPAQUE_CALLBACK);
		if (itree)
			fprintf(stderr, "[ OK ] < \"%s\"\n", sname?sname:"<stdin>");
		else
			fprintf(stderr, "[ERR ] \"%s\" does not contain valid XML.\n", sname?sname:"<stdin>");
		if (sname) 
			fclose(fp);
	}
	return itree;
}

int main(int argc, const char * const argv[]) {
	FILE *fp = NULL;
	const char *iname = NULL;
	const char *oname = NULL;
	const char *dname = NULL;
	bool requiredOnly = true;

	for(int i=1; i<argc; i++) {
		if (argv[i][0] == '-' && argv[i][1]) {
			if (argv[i][1] == 'o' && (i+1) < argc) {
				dname = argv[++i];
			} else if (argv[i][1] == 'i' && (i+1) < argc) {
				iname = argv[++i];
			} else if (argv[i][1] == 'a') {
				requiredOnly = false;
				fprintf(stderr, "[INFO] Dumping all known data-sets.\n");
			} else {
				fprintf(stderr,
					"Scade-Model I/O types to TRDP datasets mapping bridge.\n"
					"\ttypebridge [-i path/to/%s] [-o trdp-dataset-output.xml] [operator-name]\n"
					"\tUse STDIN or the -i parameter to feed in the generated mapping.xml file of KCG. The tool will"
					" write to STDOUT, if no out-file provided via -o output.xml\n"
					"\tYou can provide a designated operator name as parameter. Otherwise this tool will search for the"
					" specified root-operator.\n",
						SCADE_MAP_DEFAULT
					);
				exit(-1);
			}
		}
	}

/*
 * 1. find root node
 * 2. find root periodicity
 * 3. find input + output parameters
 * 4. resolve parameter types
 */

	mxml_node_t *itree   = readMapFile( iname );
	scanTypes(itree);

	for(int i=1; i<argc; i++) {
		if (argv[i][0]!='-' && !strchr(argv[i], '/') && !strchr(argv[i], '.')) {
			oname = argv[i];
			mxml_node_t *op  = findOperator(itree, oname);
			/*mxml_node_t *in  = */getInputsForOperator(op);
			/*mxml_node_t *out = */getOutputsForOperator(op);
		}
	}
	if ( !oname ) {
		oname  = getRootName(itree);
		mxml_node_t *rootop  = findOperator(itree, oname);
		/*mxml_node_t *in  = */getInputsForOperator(rootop); /* the required-flags will be set anyway */
		/*mxml_node_t *out = */getOutputsForOperator(rootop);
	}

	mxml_node_t *dataSetList = resolveTRDPDataSets( requiredOnly );

	/* write out the TRDP file, generally to stdout */
	if (mxmlGetFirstChild(dataSetList)) {
		mxml_node_t *otree = mxmlNewXML("1.0");
		mxmlAdd(otree, MXML_ADD_BEFORE, MXML_ADD_TO_PARENT, dataSetList);

		if (dname) {
			fp = fopen(dname, "w");
			if (!fp) {
				fprintf(stderr, "[ERR ] Could not open \"%s\" for writing.\n", dname);
				perror(NULL);
			}
			if (mxmlSaveFile(otree, fp, MXML_NO_CALLBACK) == 0 && fclose(fp) == 0) {
				fprintf(stderr, "[ OK ] Finished writing to \"%s\". Bye.\n\n", dname);
			}
		} else {
			fprintf(stderr, "[ OK ] Writing to stdout pipe.\n\n");
			mxmlSaveFile(otree, stdout, MXML_NO_CALLBACK);
		}
		mxmlDelete(otree);
	} else {
		fprintf(stderr, "[WARN] No data-sets to export. Bye.\n");
	}

	mxmlDelete(itree);
	return 0;
}
