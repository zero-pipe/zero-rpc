/* 服务声明表：进程级共享契约注册表。
 *
 * 声明通常在创建任何节点之前安装，之后只读；因此这里不加锁，避免给热路径
 * 增加开销。请在启动阶段完成 install。
 */
#include <service_table.h>
#include <types.h>

#include <string.h>

#define MAX_SERVICE_TABLES 16

static const service_table_t *g_tables[MAX_SERVICE_TABLES];
static size_t g_table_count;

static const method_descriptor_t *find_method(const char *service, const char *method)
{
    size_t t;
    for (t = 0; t < g_table_count; t++) {
        const service_table_t *table = g_tables[t];
        size_t s;
        for (s = 0; s < table->service_count; s++) {
            const service_descriptor_t *svc = &table->services[s];
            size_t m;
            if (strcmp(svc->name, service) != 0) {
                continue;
            }
            for (m = 0; m < svc->method_count; m++) {
                if (strcmp(svc->methods[m].name, method) == 0) {
                    return &svc->methods[m];
                }
            }
        }
    }
    return NULL;
}

/* ---- 安装 ---- */

int install_service_table(const service_table_t *table)
{
    size_t i;
    if (!table || (!table->services && table->service_count > 0)) {
        return ERR_INVALID;
    }
    /* 幂等：已经装过的同一张表直接成功。 */
    for (i = 0; i < g_table_count; i++) {
        if (g_tables[i] == table) {
            return OK;
        }
    }
    /* 校验声明不与已安装表冲突（同名不同 ID 或重复 ID）。 */
    for (i = 0; i < g_table_count; i++) {
        const service_table_t *other = g_tables[i];
        size_t s;
        for (s = 0; s < table->service_count; s++) {
            const service_descriptor_t *a = &table->services[s];
            size_t o;
            for (o = 0; o < other->service_count; o++) {
                const service_descriptor_t *b = &other->services[o];
                size_t m;
                if (strcmp(a->name, b->name) != 0) {
                    continue;
                }
                for (m = 0; m < a->method_count; m++) {
                    const method_descriptor_t *bm = NULL;
                    size_t n;
                    for (n = 0; n < b->method_count; n++) {
                        if (strcmp(a->methods[m].name, b->methods[n].name) == 0) {
                            bm = &b->methods[n];
                            break;
                        }
                    }
                    if (bm && bm->id != a->methods[m].id) {
                        return ERR_INVALID;
                    }
                }
            }
        }
    }
    if (g_table_count >= MAX_SERVICE_TABLES) {
        return ERR_NOMEM;
    }
    g_tables[g_table_count++] = table;
    return OK;
}

/* ---- 状态与查询 ---- */

void reset_service_table(void)
{
    g_table_count = 0;
}

int service_table_active(void)
{
    return g_table_count > 0;
}

int service_table_find_id(const char *service, const char *method, uint16_t *out_id)
{
    const method_descriptor_t *found;
    if (!service || !method) {
        return ERR_INVALID;
    }
    found = find_method(service, method);
    if (!found) {
        return ERR_NOTFOUND;
    }
    if (out_id) {
        *out_id = found->id;
    }
    return OK;
}
