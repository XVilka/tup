#define _GNU_SOURCE
#define _ATFILE_SOURCE
#include "parser.h"
#include "list.h"
#include "flist.h"
#include "fileio.h"
#include "fslurp.h"
#include "db.h"
#include "memdb.h"
#include "vardb.h"
#include "graph.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

struct name_list {
	struct list_head entries;
	int num_entries;
	int totlen;
	int extlesstotlen;
	int basetotlen;
	int extlessbasetotlen;
};

struct name_list_entry {
	struct list_head list;
	char *path;
	char *base;
	int len;
	int extlesslen;
	int baselen;
	int extlessbaselen;
	tupid_t tupid;
};

struct rule {
	struct list_head list;
	int foreach;
	char *input_pattern;
	char *output_pattern;
	char *command;
	struct name_list namelist;
	const char *dir; /* Only valid for use in file_match() */
	int dirlen; /* Same as above */
	tupid_t dt;
};

static int parse_tupfile(struct buf *b, struct vardb *vdb,
			 struct list_head *rules, int dfd, tupid_t tupid);
static int parse_rule(char *p, struct list_head *rules);
static int parse_varsed(char *p, struct list_head *rules);
static int execute_rules(struct list_head *rules, tupid_t dt, struct graph *g);
static int file_match(void *rule, struct db_node *dbn);
static char *set_path(const char *name, const char *dir, int dirlen);
static int do_rule(struct rule *r, struct name_list *nl);
static void init_name_list(struct name_list *nl);
static void set_nle_base(struct name_list_entry *nle);
static void add_name_list_entry(struct name_list *nl,
				struct name_list_entry *nle);
static void delete_name_list_entry(struct name_list *nl,
				   struct name_list_entry *nle);
static char *tup_printf(const char *cmd, struct name_list *nl,
			struct name_list *onl);
static char *eval(struct vardb *v, const char *string, tupid_t tupid);

int parse(struct node *n, struct graph *g)
{
	int dfd;
	int fd;
	int rc = -1;
	tupid_t parent;
	int num_dotdots;
	struct buf b;
	struct vardb vdb;
	struct rule *r;
	LIST_HEAD(rules);

	printf("[33mParse(%lli) %s[0m\n", n->tupid, n->name);
/*
TODO: How to find circular deps?
	if(n->state == ) {
		fprintf(stderr, "Error: Circular dependency found among Tupfiles (last tupid = %lli).\nThis is madness!\n", tupid);
		return -1;
	}
*/
	if(vardb_init(&vdb) < 0)
		return -1;
	parent = n->tupid;
	num_dotdots = 0;
	while(parent != DOT_DT) {
		parent = tup_db_parent(parent);
		num_dotdots++;
	}
	if(num_dotdots) {
		char *path = malloc(num_dotdots * 3 + 1);
		char *p;
		if(!path) {
			perror("malloc");
			return -1;
		}
		p = path;
		for(; num_dotdots; num_dotdots--) {
			strcpy(p, "../");
			p += 3;
		}
		if(vardb_set(&vdb, "TUP_TOP", path) < 0)
			return -1;
		free(path);
	} else {
		if(vardb_set(&vdb, "TUP_TOP", "") < 0)
			return -1;
	}

	/* Move all existing commands over to delete - then the ones that are
	 * re-created will be moved back out in when parsing the Tupfile. All
	 * those that are no longer generated remain in delete for cleanup.
	 *
	 * Also delete links to our directory from directories that we depend
	 * on. These will be re-generated when the file is parsed, or when
	 * the database is rolled back in case of error.
	 */
	if(tup_db_or_dircmd_flags(n->tupid, TUP_FLAGS_DELETE, TUP_NODE_CMD) < 0)
		return -1;
	if(tup_db_set_cmd_output_flags(n->tupid, TUP_FLAGS_DELETE) < 0)
		return -1;
	if(tup_db_delete_dependent_dir_links(n->tupid) < 0)
		return -1;

	dfd = tup_db_opendir(n->tupid);
	if(dfd < 0) {
		fprintf(stderr, "Error: Unable to open directory ID %lli\n", n->tupid);
		goto out_close_vdb;
	}

	fd = openat(dfd, "Tupfile", O_RDONLY);
	/* No Tupfile means we have nothing to do */
	if(fd < 0) {
		rc = 0;
		goto out_close_dfd;
	}

	if((rc = fslurp(fd, &b)) < 0) {
		goto out_close_file;
	}
	rc = parse_tupfile(&b, &vdb, &rules, dfd, n->tupid);
	if(rc < 0)
		goto out_free_bs;
	rc = execute_rules(&rules, n->tupid, g);
out_free_bs:
	free(b.s);
	while(!list_empty(&rules)) {
		r = list_entry(rules.next, struct rule, list);
		list_del(&r->list);
		free(r->input_pattern);
		free(r->output_pattern);
		free(r->command);
		free(r);
	}
out_close_file:
	close(fd);
out_close_vdb:
	if(vardb_close(&vdb) < 0)
		rc = -1;
out_close_dfd:
	close(dfd);

	if(rc == 0) {
		if(tup_db_set_flags_by_id(n->tupid, TUP_FLAGS_NONE) < 0)
			return -1;
		printf("[34mParsed(%lli) %s[0m\n", n->tupid, n->name);
	}

	return rc;
}

static int parse_tupfile(struct buf *b, struct vardb *vdb,
			 struct list_head *rules, int dfd, tupid_t tupid)
{
	char *p, *e;
	char *line;
	char *eval_line;
	int if_true = 1;

	p = b->s;
	e = b->s + b->len;

	while(p < e) {
		char *nl;
		while(isspace(*p) && p < e)
			p++;

		line = p;
		nl = strchr(p, '\n');
		if(!nl)
			goto syntax_error;
		*nl = 0;
		p = nl + 1;

		if(line[0] == '#') {
			/* Skip comments */
			continue;
		}

		if(if_true) {
			eval_line = eval(vdb, line, tupid);
		} else {
			eval_line = strdup(line);
			if(!eval_line) {
				perror("strdup");
			}
		}
		if(!eval_line)
			return -1;

		if(strncmp(eval_line, "include ", 8) == 0 ||
		   strncmp(eval_line, "include_root ", 13) == 0) {
			struct buf incb;
			int incdirfd;
			int fd;
			int rc;
			int include_root = (eval_line[7] == '_');
			char *last_slash;
			char *file;

			if(include_root)
				file = eval_line + 13;
			else
				file = eval_line + 8;
			last_slash = strrchr(file, '/');
			if(last_slash) {
				*last_slash = 0;
				if(include_root)
					incdirfd = open(file, O_RDONLY);
				else
					incdirfd = openat(dfd, file, O_RDONLY);
				if(incdirfd < 0) {
					perror(file);
					return -1;
				}
				file = last_slash + 1;
			} else {
				if(include_root) {
					incdirfd = open(".", O_RDONLY);
					if(incdirfd < 0) {
						perror(".");
						return -1;
					}
				} else {
					incdirfd = dfd;
				}
			}

			fd = openat(incdirfd, file, O_RDONLY);
			if(fd < 0) {
				fprintf(stderr, "Error including '%s': %s\n", eval_line+8, strerror(errno));
				return -1;
			}
			rc = fslurp(fd, &incb);
			close(fd);
			if(rc < 0) {
				fprintf(stderr, "Error slurping file.\n");
				return -1;
			}

			rc = parse_tupfile(&incb, vdb, rules, incdirfd, tupid);
			if(incdirfd != dfd) {
				close(incdirfd);
			}
			free(incb.s);
			if(rc < 0) {
				fprintf(stderr, "Error parsing included file '%s'\n", eval_line);
				return -1;
			}
		} else if(strcmp(eval_line, "else") == 0) {
			if_true = !if_true;
		} else if(strcmp(eval_line, "endif") == 0) {
			/* TODO: Nested if */
			if_true = 1;
		} else if(!if_true) {
			/* Skip the false part of an if block */
		} else if(strncmp(eval_line, "ifeq ", 5) == 0) {
			char *paren;
			char *comma;
			char *lval;
			char *rval;

			paren = strchr(eval_line+5, '(');
			if(!paren)
				goto syntax_error;
			lval = paren + 1;
			comma = strchr(lval, ',');
			if(!comma)
				goto syntax_error;
			rval = comma + 1;
			paren = strchr(rval, ')');
			if(!paren)
				goto syntax_error;
			*comma = 0;
			*paren = 0;

			if(strcmp(lval, rval) == 0) {
				if_true = 1;
			} else {
				if_true = 0;
			}
		} else if(eval_line[0] == ':') {
			if(parse_rule(eval_line+1, rules) < 0)
				goto syntax_error;
		} else if(eval_line[0] == ',') {
			if(parse_varsed(eval_line+1, rules) < 0)
				goto syntax_error;
		} else {
			char *eq;
			char *value;
			int append;

			/* Find the += or = sign, and point value to the start
			 * of the string after that op.
			 */
			eq = strstr(eval_line, "+=");
			if(eq) {
				value = eq + 2;
				append = 1;
			} else {
				eq = strstr(eval_line, ":=");
				if(eq) {
					value = eq + 2;
					append = 0;
				} else {
					eq = strchr(eval_line, '=');
					if(!eq)
						goto syntax_error;
					value = eq + 1;
					append = 0;
				}
			}

			/* End the lval with a 0, then space-delete the end
			 * of the variable name and the beginning of the value.
			 */
			*eq = 0;
			while(isspace(*value) && *value != 0)
				value++;
			eq--;
			while(isspace(*eq) && eq > eval_line) {
				*eq = 0;
				eq--;
			}

			if(append)
				vardb_append(vdb, eval_line, value);
			else
				vardb_set(vdb, eval_line, value);
		}

		free(eval_line);
	}

	return 0;

syntax_error:
	fprintf(stderr, "Syntax error parsing Tupfile\n  Line was: %s\n", line);
	return -1;
}

static int parse_rule(char *p, struct list_head *rules)
{
	char *input, *cmd, *output;
	char *ie, *ce;
	struct rule *r;

	input = p;
	while(isspace(*input))
		input++;
	p = strstr(p, "|>");
	if(!p)
		return -1;
	if(input < p) {
		ie = p - 1;
		while(isspace(*ie))
			ie--;
		ie[1] = 0;
	} else {
		input = NULL;
	}
	p += 2;
	cmd = p;
	while(isspace(*cmd))
		cmd++;
	p = strstr(p, "|>");
	if(!p)
		return -1;
	ce = p - 1;
	while(isspace(*ce))
		ce--;
	p += 2;
	output = p;
	while(isspace(*output))
		output++;
	ce[1] = 0;

	r = malloc(sizeof *r);
	if(!r) {
		perror("malloc");
		return -1;
	}
	if(input) {
		r->foreach = 0;
		if(strncmp(input, "foreach ", 8) == 0) {
			r->foreach = 1;
			input += 8;
		}
		r->input_pattern = strdup(input);
	} else {
		r->input_pattern = strdup("");
		r->foreach = 0;
	}
	if(!r->input_pattern) {
		perror("strdup");
		return -1;
	}
	r->output_pattern = strdup(output);
	if(!r->output_pattern) {
		perror("strdup");
		return -1;
	}
	r->command = strdup(cmd);
	if(!r->command) {
		perror("strdup");
		return -1;
	}
	init_name_list(&r->namelist);

	list_add_tail(&r->list, rules);

	return 0;
}

static int parse_varsed(char *p, struct list_head *rules)
{
	char *input, *output;
	char *ie;
	struct rule *r;

	input = p;
	while(isspace(*input))
		input++;
	p = strstr(p, "|>");
	if(!p)
		return -1;
	if(input < p) {
		ie = p - 1;
		while(isspace(*ie))
			ie--;
		ie[1] = 0;
	} else {
		input = NULL;
	}
	p += 2;
	output = p;
	while(isspace(*output))
		output++;

	r = malloc(sizeof *r);
	if(!r) {
		perror("malloc");
		return -1;
	}
	r->foreach = 1;
	r->input_pattern = strdup(input);
	if(!r->input_pattern) {
		perror("strdup");
		return -1;
	}
	r->output_pattern = strdup(output);
	if(!r->output_pattern) {
		perror("strdup");
		return -1;
	}

	r->command = strdup(", %f > %o");
	if(!r->command) {
		perror("strdup");
		return -1;
	}

	init_name_list(&r->namelist);
	list_add_tail(&r->list, rules);
	return 0;
}

static int execute_rules(struct list_head *rules, tupid_t dt, struct graph *g)
{
	struct rule *r;

	list_for_each_entry(r, rules, list) {
		char *spc;
		char *p;
		const char *file;
		tupid_t subdir;

		r->dt = dt;
		p = r->input_pattern;
		do {
			/* Blank input pattern */
			if(!p[0])
				break;

			spc = strchr(p, ' ');
			if(spc)
				*spc = 0;

			subdir = find_dir_tupid_dt(dt, p, &file);
			if(subdir < 0) {
				fprintf(stderr, "Error: Failed to find directory ID for dir '%s'\n", p);
				return -1;
			}
			if(subdir != dt) {
				struct node *n;
				if(memdb_find(&g->memdb, subdir, &n) < 0)
					return -1;
				if(n != NULL) {
					if(parse(n, g) < 0)
						return -1;
				}
				if(tup_db_create_link(subdir, dt) < 0)
					return -1;
			}
			if(p != file) {
				/* Note that dirlen should be file-p-1, but we
				 * add 1 to account for the trailing '/' that
				 * will be added.
				 */
				p[file-p-1] = 0;
				r->dir = p;
				r->dirlen = file-p;
			} else {
				r->dir = "";
				r->dirlen = 0;
			}
			if(strchr(file, '*') == NULL) {
				struct db_node dbn;
				if(tup_db_select_dbn(subdir, file, &dbn) < 0) {
					fprintf(stderr, "Error: Explicitly named file '%s' not found in subdir %lli.\n", file, subdir);
					return -1;
				}
				if(file_match(r, &dbn) < 0)
					return -1;
			} else {
				if(tup_db_select_node_dir_glob(file_match, r, subdir, file) < 0)
					return -1;
			}

			if(spc)
				p = spc + 1;
		} while(spc != NULL);

		/* Only parse non-foreach rules if the namelist has some
		 * entries, or if there is no input listed. We don't want to
		 * generate a command if there is an input pattern but no
		 * entries match (for example, *.o inputs to ld %f with no
		 * object files). However, if you have no input but just a
		 * command (eg: you want to run a shell script), then we still
		 * want to do the rule for that case.
		 */
		if(!r->foreach && (r->namelist.num_entries > 0 ||
				   strcmp(r->input_pattern, "") == 0)) {
			struct name_list_entry *nle;

			if(do_rule(r, &r->namelist) < 0)
				return -1;

			while(!list_empty(&r->namelist.entries)) {
				nle = list_entry(r->namelist.entries.next,
						struct name_list_entry, list);
				delete_name_list_entry(&r->namelist, nle);
			}
		}
	}
	return 0;
}

static int file_match(void *rule, struct db_node *dbn)
{
	struct rule *r = rule;
	int extlesslen;
	int len;
	int namelen;

	namelen = strlen(dbn->name);
	len = namelen + r->dirlen;
	extlesslen = namelen - 1;
	while(extlesslen > 0 && dbn->name[extlesslen] != '.')
		extlesslen--;
	if(extlesslen == 0)
		extlesslen = namelen;
	extlesslen += r->dirlen;

	if(r->foreach) {
		struct name_list nl;
		struct name_list_entry nle;

		nle.path = set_path(dbn->name, r->dir, r->dirlen);
		if(!nle.path)
			return -1;

		nle.len = len;
		nle.extlesslen = extlesslen;
		nle.tupid = dbn->tupid;
		set_nle_base(&nle);

		init_name_list(&nl);
		add_name_list_entry(&nl, &nle);

		if(do_rule(r, &nl) < 0)
			return -1;
		free(nle.path);
	} else {
		struct name_list_entry *nle;

		nle = malloc(sizeof *nle);
		if(!nle) {
			perror("malloc");
			return -1;
		}

		nle->path = set_path(dbn->name, r->dir, r->dirlen);
		if(!nle->path)
			return -1;

		nle->len = len;
		nle->extlesslen = extlesslen;
		nle->tupid = dbn->tupid;
		set_nle_base(nle);

		add_name_list_entry(&r->namelist, nle);
	}
	return 0;
}

static char *set_path(const char *name, const char *dir, int dirlen)
{
	char *path;

	if(dirlen) {
		int nlen;

		nlen = strlen(name);
		/* +1 for '/', +1 for nul */
		path = malloc(nlen + dirlen + 1);
		if(!path) {
			perror("malloc");
			return NULL;
		}

		memcpy(path, dir, dirlen-1);
		path[dirlen-1] = '/';
		strcpy(path + dirlen, name);
	} else {
		path = strdup(name);
		if(!path) {
			perror("strdup");
			return NULL;
		}
	}
	return path;
}

static int do_rule(struct rule *r, struct name_list *nl)
{
	struct name_list onl;
	struct name_list_entry *nle, *tmp, *onle;
	char *p;
	char *spc;
	char *cmd;
	int node_created = 0;
	tupid_t cmd_id;

	init_name_list(&onl);

	p = r->output_pattern;
	do {
		spc = strchr(p, ' ');
		if(spc)
			*spc = 0;

		onle = malloc(sizeof *onle);
		if(!onle) {
			perror("malloc");
			return -1;
		}
		onle->path = tup_printf(p, nl, NULL);
		if(!onle->path)
			return -1;
		if(strchr(onle->path, '/')) {
			fprintf(stderr, "Error: Attempted to create an output file '%s', which contains a '/' character. Tupfiles should only output files in their own directories.\n - Directory: %lli\n - Rule: '%s [33m|>[0m [35m%s[0m [33m|>[0m %s'\n", onle->path, r->dt, r->input_pattern, r->command, r->output_pattern);
			return -1;
		}
		onle->len = strlen(onle->path);
		onle->extlesslen = onle->len - 1;
		while(onle->extlesslen > 0 && onle->path[onle->extlesslen] != '.')
			onle->extlesslen--;

		onle->tupid = tup_db_create_node_part(r->dt, onle->path, -1,
						      TUP_NODE_FILE,
						      TUP_FLAGS_MODIFY,
						      &node_created);
		if(onle->tupid < 0)
			return -1;

		add_name_list_entry(&onl, onle);

		if(spc)
			p = spc + 1;
	} while(spc != NULL);

	list_for_each_entry_safe(nle, tmp, &nl->entries, list) {
		list_for_each_entry(onle, &onl.entries, list) {
			if(nle->tupid == onle->tupid) {
				fprintf(stderr, "Error: Attempting to use a command's output as its input in dir ID %lli. Output ID %lli is '%s'. Deleting entry from input list\n", r->dt, onle->tupid, onle->path);
				delete_name_list_entry(&r->namelist, nle);
			}
		}
	}

	cmd = tup_printf(r->command, nl, &onl);
	if(!cmd)
		return -1;
	cmd_id = create_command_file(r->dt, cmd);
	free(cmd);
	if(cmd_id < 0)
		return -1;

	while(!list_empty(&onl.entries)) {
		onle = list_entry(onl.entries.next, struct name_list_entry,
				  list);
		if(tup_db_create_link(cmd_id, onle->tupid) < 0)
			return -1;
		delete_name_list_entry(&onl, onle);
	}

	list_for_each_entry(nle, &nl->entries, list) {
		if(tup_db_create_link(nle->tupid, cmd_id) < 0)
			return -1;
	}
	return 0;
}

static void init_name_list(struct name_list *nl)
{
	INIT_LIST_HEAD(&nl->entries);
	nl->num_entries = 0;
	nl->totlen = 0;
	nl->extlesstotlen = 0;
	nl->basetotlen = 0;
	nl->extlessbasetotlen = 0;
}

static void set_nle_base(struct name_list_entry *nle)
{
	nle->base = nle->path + nle->len;
	nle->baselen = 0;
	while(nle->base > nle->path) {
		nle->base--;
		if(nle->base[0] == '/') {
			nle->base++;
			goto out;
		}
		nle->baselen++;
	}
out:
	/* The extension-less baselen is the length of the base, minus the
	 * length of the extension we calculated before.
	 */
	nle->extlessbaselen = nle->baselen - (nle->len - nle->extlesslen);
}

static void add_name_list_entry(struct name_list *nl,
				struct name_list_entry *nle)
{
	list_add(&nle->list, &nl->entries);
	nl->num_entries++;
	nl->totlen += nle->len;
	nl->extlesstotlen += nle->extlesslen;
	nl->basetotlen += nle->baselen;
	nl->extlessbasetotlen += nle->extlessbaselen;
}

static void delete_name_list_entry(struct name_list *nl,
				   struct name_list_entry *nle)
{
	nl->num_entries--;
	nl->totlen -= nle->len;
	nl->extlesstotlen -= nle->extlesslen;
	nl->basetotlen -= nle->baselen;
	nl->extlessbasetotlen -= nle->extlessbaselen;

	list_del(&nle->list);
	free(nle->path);
	free(nle);
}

static char *tup_printf(const char *cmd, struct name_list *nl,
			struct name_list *onl)
{
	struct name_list_entry *nle;
	char *s;
	int x;
	const char *p;
	const char *next;
	const char *spc;
	int clen = strlen(cmd);

	p = cmd;
	while((p = strchr(p, '%')) !=  NULL) {
		int paste_chars;

		clen -= 2;
		p++;
		spc = p + 1;
		while(*spc && *spc != ' ')
			spc++;
		paste_chars = (nl->num_entries - 1) * (spc - p);
		if(*p == 'f') {
			clen += nl->totlen + paste_chars;
		} else if(*p == 'F') {
			clen += nl->extlesstotlen + paste_chars;
		} else if(*p == 'b') {
			clen += nl->basetotlen + paste_chars;
		} else if(*p == 'B') {
			clen += nl->extlessbasetotlen + paste_chars;
		} else if(*p == 'o') {
			if(!onl) {
				fprintf(stderr, "Error: %%o can only be used in a command.\n");
				return NULL;
			}
			clen += onl->totlen + (onl->num_entries-1);
		}
	}

	s = malloc(clen + 1);
	if(!s) {
		perror("malloc");
		return NULL;
	}

	p = cmd;
	x = 0;
	while((next = strchr(p, '%')) !=  NULL) {
		memcpy(&s[x], p, next-p);
		x += next-p;

		next++;
		p = next + 1;
		spc = p;
		while(*spc && *spc != ' ')
			spc++;
		if(*next == 'f') {
			int first = 1;
			list_for_each_entry(nle, &nl->entries, list) {
				if(!first) {
					s[x] = ' ';
					x++;
				}
				memcpy(&s[x], nle->path, nle->len);
				x += nle->len;
				memcpy(&s[x], p, spc - p);
				x += spc - p;
				first = 0;
			}
		} else if(*next == 'F') {
			int first = 1;
			list_for_each_entry(nle, &nl->entries, list) {
				if(!first) {
					s[x] = ' ';
					x++;
				}
				memcpy(&s[x], nle->path, nle->extlesslen);
				x += nle->extlesslen;
				memcpy(&s[x], p, spc - p);
				x += spc - p;
				first = 0;
			}
		} else if(*next == 'b') {
			int first = 1;
			list_for_each_entry(nle, &nl->entries, list) {
				if(!first) {
					s[x] = ' ';
					x++;
				}
				memcpy(&s[x], nle->base, nle->baselen);
				x += nle->baselen;
				memcpy(&s[x], p, spc - p);
				x += spc - p;
				first = 0;
			}
		} else if(*next == 'B') {
			int first = 1;
			list_for_each_entry(nle, &nl->entries, list) {
				if(!first) {
					s[x] = ' ';
					x++;
				}
				memcpy(&s[x], nle->base, nle->extlessbaselen);
				x += nle->extlessbaselen;
				memcpy(&s[x], p, spc - p);
				x += spc - p;
				first = 0;
			}
		} else if(*next == 'o') {
			int first = 1;
			list_for_each_entry(nle, &onl->entries, list) {
				if(!first) {
					s[x] = ' ';
					x++;
				}
				memcpy(&s[x], nle->path, nle->len);
				x += nle->len;
				first = 0;
			}
		}
		p = spc;
	}
	strcpy(&s[x], p);
	if((signed)strlen(s) != clen) {
		fprintf(stderr, "Error: Calculated string length (%i) didn't match actual (%i). String is: '%s'.\n", clen, strlen(s), s);
		return NULL;
	}
	return s;
}

static char *eval(struct vardb *v, const char *string, tupid_t tupid)
{
	int len = 0;
	char *ret;
	char *p;
	const char *s;
	const char *var;
	const char *expected = "oops";
	int vlen;

	s = string;
	while(*s) {
		if(*s == '$') {
			const char *rparen;

			if(s[1] != '(') {
				expected = "$(";
				goto syntax_error;
			}
			rparen = strchr(s+1, ')');
			if(!rparen) {
				expected = "ending variable paren ')'";
				goto syntax_error;
			}

			var = s + 2;
			vlen = vardb_len(v, var, rparen-var);
			if(vlen < 0)
				return NULL;
			len += vlen;
			s = rparen + 1;
		} else if(*s == '@') {
			const char *rat;

			rat = strchr(s+1, '@');
			if(!rat) {
				expected = "ending @-symbol";
				goto syntax_error;
			}

			var = s + 1;
			vlen = tup_db_get_varlen(var, rat-s-1);
			if(vlen < 0)
				return NULL;
			len += vlen;
			s = rat + 1;
		} else {
			s++;
			len++;
		}
	}

	ret = malloc(len+1);
	if(!ret) {
		perror("malloc");
		return NULL;
	}

	p = ret;
	s = string;
	while(*s) {
		if(*s == '$') {
			const char *rparen;

			if(s[1] != '(') {
				expected = "$(";
				goto syntax_error;
			}
			rparen = strchr(s+1, ')');
			if(!rparen) {
				expected = "ending variable paren ')'";
				goto syntax_error;
			}

			var = s + 2;
			if(vardb_get(v, var, rparen-var, &p) < 0)
				return NULL;

			s = rparen + 1;
		} else if(*s == '@') {
			const char *rat;
			tupid_t vt;

			rat = strchr(s+1, '@');
			if(!rat) {
				expected = "ending @-symbol";
				goto syntax_error;
			}

			var = s + 1;
			vt = tup_db_get_var(var, rat-s-1, &p);
			if(vt < 0)
				return NULL;
			if(tup_db_create_link(vt, tupid) < 0)
				return NULL;

			s = rat + 1;
		} else {
			*p = *s;
			p++;
			s++;
		}
	}
	strcpy(p, s);

	if((signed)strlen(ret) != len) {
		fprintf(stderr, "Length mismatch: expected %i bytes, wrote %i\n", len, strlen(ret));
	}

	return ret;

syntax_error:
	fprintf(stderr, "Syntax error: expected %s\n", expected);
	return NULL;
}
