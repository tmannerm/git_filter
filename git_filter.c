/*
 * Copyright (C) 2014 Brian Murphy <brian@murphy.dk>
 *
 * This file is part of git_filter, distributed under the GNU GPL v2.
 * For full terms see the included COPYING file.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "git2.h"

#include "git_filter.h"
#include "dict.h"

#define STACK_CHUNKS 32
#define INCLUDE_CHUNKS 1024
#define TAG_INFO_CHUNKS 128
#define TF_LIST_CHUNKS 32

#define C(git2_call) do { \
    int _error = git2_call; \
    if (_error < 0) \
    { \
        const git_error *e = giterr_last(); \
        log("error %d at %d calling %s\n", _error, __LINE__, #git2_call); \
        log("%d: %s\n", e->klass, e->message); \
        exit(1); \
    } \
} while(0)

#define tree_equal(tree1, tree2) git_oid_equal(git_tree_id(tree1), \
            git_tree_id(tree2))

struct include_dirs {
    char **dirs;
    unsigned int alloc;
    unsigned int len;
};

#define BUFLEN 128
char *local_sprintf(const char *format, ...)
{
    va_list ap;
    int cnt;
    char *out = malloc(BUFLEN);

    A(out == 0, "no memory");

    va_start(ap, format);
    cnt = vsnprintf(out, BUFLEN, format, ap);
    va_end(ap);

    if (cnt > BUFLEN)
    {
        cnt ++;
        out = realloc(out, cnt);
        A(out == 0, "no memory");

        va_start(ap, format);
        vsnprintf(out, cnt, format, ap);
        va_end(ap);
    }

    return out;
}

char *local_fgets(FILE *f)
{
    char *line = 0;
    char *r;
    unsigned int len = BUFLEN;
    size_t slen = 0;

    do {
        line = realloc(line, len);
        A(line == 0, "no memory");

        r = fgets(&line[slen], len-slen, f);
        if (!r)
        {
            if (slen != 0)
                break;
            free(line);
            return 0;
        }
        slen = strlen(line);
        len += BUFLEN;
    } while(line[slen-1] != '\n');

    line[slen - 1] = 0;
    return line;
}

void _rev_info_dump(void *d, const void *k, const void *v)
{
    FILE *f = (FILE *)d;
    char oids1[GIT_OID_HEXSZ+1];
    char oids2[GIT_OID_HEXSZ+1];
    const git_oid *o = (const git_oid *)k;
    const git_oid *cid = git_commit_id((const git_commit *)v);

    fprintf(f, "%s: %s\n",
            git_oid_tostr(oids1, GIT_OID_HEXSZ+1, o),
            git_oid_tostr(oids2, GIT_OID_HEXSZ+1, cid)
           );
}

void rev_info_dump(dict_t *d, const char *filename)
{
    FILE *f;
    char *full_path = local_sprintf("%s.revinfo", filename);

    f = fopen(full_path, "w");
    if (!f)
        die("cannot open %s\n", filename);

    dict_dump(d, _rev_info_dump, f);

    fclose(f);

    free(full_path);
}

int oid_cmp(const void *k1, const void *k2)
{
    const git_oid *s1 = (const git_oid *)k1;
    const git_oid *s2 = (const git_oid *)k2;

    return git_oid_cmp(s1, s2);
}


struct tree_filter {
    const char *name;;
    const char *include_file;
    struct include_dirs id;

    /* TODO fix tagging reconstruction and remove these */
    git_commit *last;

    git_repository *repo;
    dict_t *revdict;
};

char *git_repo_name = 0;
char *git_tag_prefix = 0;
char *rev_type = 0;
char *rev_string = 0;

unsigned int tf_len = 0;
struct tree_filter *tf_list;
unsigned int tf_list_alloc = 0;

#define CHECK_FILES 1

#if CHECK_FILES
/* string sorting callback for qsort */
int sort_string(const void *a, const void *b)
{
    const char **stra = (const char **)a;
    const char **strb = (const char **)b;

    return strcmp(*stra, *strb);
}
#endif

void include_dirs_init(struct include_dirs *id, const char *file)
{
    FILE *f;

    id->dirs = malloc(sizeof (char *) * INCLUDE_CHUNKS);
    id->alloc = INCLUDE_CHUNKS;

    f = fopen(file, "r");
    if (!f)
        die("cannot open %s\n", file);

    while(!feof(f))
    {
        char *e = local_fgets(f);
        if (!e)
            break;
        if (id->len == id->alloc)
        {
            id->alloc += INCLUDE_CHUNKS;
            id->dirs = realloc(id->dirs,
                    id->alloc * sizeof (char *));
        }

        id->dirs[id->len] = e;
        id->len++;
    }

    fclose(f);

#if CHECK_FILES
    int i;

    qsort(id->dirs, id->len, sizeof(char *), sort_string);

    for (i=1; i<id->len; i++)
    {
        if (!strcmp(id->dirs[i], id->dirs[i-1]))
            die("duplicate entries for '%s'", id->dirs[i]);

        unsigned int cmplen = strlen(id->dirs[i-1]);
        if (!strncmp(id->dirs[i], id->dirs[i-1], cmplen)
                && id->dirs[i][cmplen] == '/')
            die("'%s' is a subdir of '%s'", id->dirs[i], id->dirs[i-1]);
    }
#endif
}

void tree_filter_init(struct tree_filter *tf, git_repository *repo)
{
    include_dirs_init(&tf->id, tf->include_file);

    tf->repo = repo;

    tf->revdict = dict_init(oid_cmp);

    A(tf->revdict == 0, "failed to allocate list");
}

void tree_filter_fini(struct tree_filter *tf)
{
}

typedef struct _dirstack_item_t {
    git_treebuilder *tb;
    char *name;
} dirstack_item_t;

typedef struct _dirstack_t {
    dirstack_item_t *item;
    unsigned int alloc;
    unsigned int depth;
    git_repository *repo;
} dirstack_t;

dirstack_item_t *stack_get_item(dirstack_t *stack, int level)
{
    if (stack->alloc <= level)
    {
        stack->alloc += STACK_CHUNKS;

        stack->item = realloc(stack->item,
                stack->alloc * sizeof(dirstack_item_t));
        A(stack->item == 0, "no memory");

        memset(&stack->item[stack->alloc-STACK_CHUNKS], 0,
                STACK_CHUNKS * sizeof(dirstack_item_t));
    }
    return &stack->item[level];
}

void _stack_close_to(dirstack_t *stack, unsigned int level)
{
    unsigned int i;

    for (i = stack->depth - 1; i >= level; i--)
    {
        dirstack_item_t *cur = stack_get_item(stack, i);
        dirstack_item_t *prev = stack_get_item(stack, i-1);
        git_oid new_oid;

        C(git_treebuilder_write(&new_oid, stack->repo, cur->tb));
        git_treebuilder_free(cur->tb);

        C(git_treebuilder_insert(0, prev->tb, cur->name,
                    &new_oid, GIT_FILEMODE_TREE));

        free(cur->name);
        cur->name = 0;
        cur->tb = 0;
    }

    stack->depth = level;
}

void _handle_stack(dirstack_t *stack, char **path_c, unsigned int len)
{
    dirstack_item_t *s;
    unsigned int level;

    if (len == 0)
        return;

    for (level = 1; level <= len; level++)
    {
        s = stack_get_item(stack, level);

        if (!s->name)
        {
            s->name = strdup(path_c[level-1]);
            C(git_treebuilder_create(&s->tb, 0));
            stack->depth = level + 1;
            continue;
        }

        if (!strcmp(s->name, path_c[level-1]))
            continue;

        A(s->tb == 0, "stack overflow\n");

        _stack_close_to(stack, level);

        A(stack->depth != level, "stack error");
        A(s->name != 0, "stack error");
        A(s->tb != 0, "stack error");

        C(git_treebuilder_create(&s->tb, 0));
        s->name = strdup(path_c[level-1]);
        stack->depth = level + 1;
    }
}

void stack_open(dirstack_t *stack, git_repository *repo)
{
    dirstack_item_t *di;
    memset(stack, 0, sizeof(*stack));

    di = stack_get_item(stack, 0);

    C(git_treebuilder_create(&di->tb, 0));
    stack->depth = 1;
    stack->repo = repo;
}


#define add_pathc(p, item) do { \
    p[cnt++] = last; \
    if (cnt == path_size) \
    { \
        *p += STACK_CHUNKS; \
        p = realloc(p, path_size * sizeof(char *)); \
        A(p == 0, "no memory"); \
    } \
} while(0)


/* modifies path */
unsigned int split_path(char ***path_sp, char *path)
{
    char *next;
    char *last = path;
    unsigned int cnt;
    unsigned int path_size = STACK_CHUNKS;
    char **p = malloc(path_size * sizeof(char *));
    A(p == 0, "no memory");

    cnt = 0;
    while ((next = strchr(last, '/')))
    {
        *next = 0;

        add_pathc(p, last);

        last = next + 1;
    }

    add_pathc(p, last);

    p[cnt] = 0;

    *path_sp = p;

    return cnt;
}

void stack_add(dirstack_t *stack, const char *path, 
        const git_tree_entry *ent)
{
    const char *name = git_tree_entry_name(ent);
    const git_oid *t_oid = git_tree_entry_id(ent);
    const git_filemode_t t_fm = git_tree_entry_filemode(ent);

    char *tmppath = strdup(path);
    char **path_sp;
    dirstack_item_t *di;

    unsigned int cnt = split_path(&path_sp, tmppath);

    path_sp[cnt-1] = 0;

    if (cnt > 1)
        _handle_stack(stack, path_sp, cnt - 1);

    di = stack_get_item(stack, cnt-1);

    C(git_treebuilder_insert(0, di->tb, name, t_oid, t_fm));

    free(path_sp);
    free(tmppath);
}

int stack_close(dirstack_t *stack, git_oid *new_oid)
{
    dirstack_item_t *di;

    _stack_close_to(stack, 1);

    di = stack_get_item(stack, 0);

    C(git_treebuilder_write(new_oid, stack->repo, di->tb));

    git_treebuilder_free(di->tb);

    free(stack->item);

    return 0;
}

git_tree *filtered_tree(struct include_dirs *id,
        git_tree *tree, git_repository *repo)
{
    git_tree *new_tree;
    git_oid new_oid;
    int i;
    dirstack_t stack;

    stack_open(&stack, repo);

    for(i=0; i<id->len; i++)
    {
        int error;
        git_tree_entry *out;
        const char *path = id->dirs[i];

        error = git_tree_entry_bypath(&out, tree, path);

        if (error == 0)
        {
            stack_add(&stack, path, out);
            git_tree_entry_free(out);
        }
    }

    C(stack_close(&stack, &new_oid));

    C(git_tree_lookup(&new_tree, repo, &new_oid));

    return new_tree;
}

#define OIDLIST_MAX 16
typedef struct _commit_list_t
{
    const git_commit *list[OIDLIST_MAX];
    unsigned int len;
} commit_list_t;

/* find the parents of the original commit and
   map them to new commits */
void find_new_parents(git_commit *old, dict_t *oid_dict, 
        commit_list_t *commit_list)
{
    int cpcount;

    cpcount = git_commit_parentcount(old);

    if (cpcount)
    {
        /* find parents */
        unsigned int n;
        for (n = 0; n < cpcount; n++)
        {
            git_commit *old_parent;
            const git_oid *old_pid;
            C(git_commit_parent(&old_parent, old, n));
            old_pid = git_commit_id(old_parent);
            const git_commit *newc = dict_lookup(oid_dict, old_pid);
            if (newc == 0)
                find_new_parents(old_parent, oid_dict, commit_list);
            else
            {
                A(commit_list->len >= OIDLIST_MAX, "too many parents");
                commit_list->list[commit_list->len] = newc;
                commit_list->len ++;
            }
        }
    }
}


void create_commit(struct tree_filter *tf, git_tree *tree,
        git_commit *commit, git_oid *commit_id)
{
    git_tree *new_tree;
    git_oid new_commit_id;
    const char *message;
    const git_signature *committer;
    const git_signature *author;
    commit_list_t commit_list;

    message = git_commit_message(commit);
    committer = git_commit_committer(commit);
    author = git_commit_author(commit);

    new_tree = filtered_tree(&tf->id, tree, tf->repo);

    if (git_tree_entrycount(new_tree) == 0)
        return;

    author = git_commit_author(commit);

    commit_list.len = 0;
    find_new_parents(commit, tf->revdict, &commit_list);
    
    /* skip commits which have identical trees but only
       in the simple case of one parent */
    if (commit_list.len == 1)
    {
        git_tree *old_tree;
        C(git_commit_tree(&old_tree, commit_list.list[0]));
        if (tree_equal(old_tree, new_tree))
            return;
    }

    C(git_commit_create(&new_commit_id,
                tf->repo, NULL,
                author, committer,
                NULL,
                message, new_tree,
                commit_list.len, commit_list.list));

    C(git_commit_lookup(&tf->last, tf->repo, &new_commit_id));

    git_oid *c_id_cp;

    c_id_cp = (git_oid *)malloc(sizeof(git_oid));
    A(c_id_cp == 0, "no memory");

    *c_id_cp = *commit_id;

    dict_add(tf->revdict, c_id_cp, tf->last);
}


#define CONFIG_KEYLEN 5
void parse_config_file(const char *cfgfile)
{
    FILE *f;
    unsigned int lineno;
    char *base = 0;

    f = fopen(cfgfile, "r");
    if (!f)
        die("cannot open %s\n", cfgfile);

    lineno = 0;
    while(!feof(f))
    {
        char *e = local_fgets(f);
        if (!e)
            break;

        lineno ++;

#define VALUE(buf) (buf+CONFIG_KEYLEN+1)

        if (e[0] == '#')
            continue;

        if (!strncmp(e, "REPO: ", CONFIG_KEYLEN))
        {
            if (git_repo_name)
                die("can only specify one repository in %s at %d\n",
                        cfgfile, lineno);
            git_repo_name = strdup(VALUE(e));
        }
        if (!strncmp(e, "TPFX: ", CONFIG_KEYLEN))
        {
            if (git_tag_prefix)
                die("can only specify one tag prefix in %s at %d\n",
                        cfgfile, lineno);
            git_tag_prefix = strdup(VALUE(e));
        }
        if (!strncmp(e, "REVN: ", CONFIG_KEYLEN))
        {
            if (rev_type)
                die("can only specify one revision in %s at %d\n",
                        cfgfile, lineno);
            rev_type = strdup(VALUE(e));
            rev_string = strchr(rev_type, ' ');
            if (!rev_string)
                die("can't find revision %s at %d\n", cfgfile, lineno);
            *rev_string = 0;
            rev_string ++;
        }
        if (!strncmp(e, "BASE: ", CONFIG_KEYLEN))
        {
            if (base)
            {
                free(base);
            }
            base = strdup(VALUE(e));
        }
        if (!strncmp(e, "FILT: ", CONFIG_KEYLEN))
        {
            char *name = strdup(VALUE(e));
            char *file = strchr(name, ' ');
            if (!file)
                die("invalid syntax for filter in %s at %d\n",
                        cfgfile, lineno);
            *file = 0;

            if (tf_len >= tf_list_alloc)
            {
                tf_list_alloc += TF_LIST_CHUNKS;
                tf_list = realloc(tf_list, tf_list_alloc * 
                        sizeof(struct tree_filter));
            }
            tf_list[tf_len].name = name;

            file ++;

            if (base)
                file = local_sprintf("%s%s", base, file);
            tf_list[tf_len].include_file = file;

            tf_len ++;
        }

        free(e);
    }

    if (rev_string == 0)
        die("no REVN: line found in %s\n", cfgfile);
    if (git_tag_prefix == 0)
        die("no TPFX: line found in %s\n", cfgfile);
    if (git_repo_name == 0)
        die("no REPO: line found in %s\n", cfgfile);
    if (tf_len == 0)
        die("no fiter specified in %s\n", cfgfile);

    fclose(f);
}


int main(int argc, char *argv[])
{
    git_repository *repo;
    git_revwalk *walker;
    git_oid commit_oid;
    unsigned int count;
    unsigned int i;

    if (argc < 2)
    {
        log("please specify the location of a filter configuration\n");
        log("%s <filter config>\n", argv[0]);
        exit(1);
    }

    parse_config_file(argv[1]);

    C(git_repository_init(&repo, git_repo_name, 0));

    for (i = 0; i < tf_len; i++)
        tree_filter_init(&tf_list[i], repo);

    C(git_revwalk_new(&walker, repo));
    git_revwalk_sorting(walker, GIT_SORT_REVERSE|GIT_SORT_TOPOLOGICAL);

    if (!strcmp(rev_type, "ref"))
    {
        C(git_revwalk_push_ref(walker, rev_string));
    }
    else if (!strcmp(rev_type, "range"))
    {
        C(git_revwalk_push_range(walker, rev_string));
    }
    else
    {
        die("invalid revision type %s in REVN", rev_type);
    }

    count = 0;
    while (!git_revwalk_next(&commit_oid, walker)) {
        git_tree *tree;
        git_commit *commit;

        C(git_commit_lookup(&commit, repo, &commit_oid));
        C(git_commit_tree(&tree, commit));

        for (i = 0; i < tf_len; i++)
            create_commit(&tf_list[i], tree, commit, &commit_oid);

        count ++;
        if (count % 1000 == 0)
            log("count %d\n", count);

        git_commit_free(commit);
        git_tree_free(tree);
    }

    for (i = 0; i < tf_len; i++)
    {
        char oid_p[GIT_OID_HEXSZ+1];
        char *tag;
        struct tree_filter *tf = &tf_list[i];
        char *n;
        const git_oid *commit_id;

        commit_id = git_commit_id(tf->last);
        n = git_oid_tostr(oid_p, GIT_OID_HEXSZ+1, commit_id);

        tag = local_sprintf("refs/heads/%s%s", git_tag_prefix, tf->name);
        C(git_reference_create(0, tf->repo, tag, commit_id, 1));
        log("final name %s as %s\n", n, tag);

        free(tag);

        rev_info_dump(tf->revdict, tf->name);

        tree_filter_fini(&tf[i]);
    }

    git_revwalk_free(walker);
    git_repository_free(repo);

    return 0;
}
