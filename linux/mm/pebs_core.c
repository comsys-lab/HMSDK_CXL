#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/huge_mm.h>
#include <linux/mm_inline.h>
#include <linux/pid.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/sched/task.h>
#include <linux/xarray.h>
#include <linux/math.h>
#include <linux/random.h>
#include <linux/slab.h>     // kmalloc, kzalloc, krealloc, kfree
#include <linux/types.h>    // u64 등
#include <linux/string.h>   // memmove
#include <linux/sort.h>     // 커널 내장 sort() 사용 가능


#include "internal.h"
#include <asm/pgtable.h>
#include <linux/pebs_test.h>

void print_all_vma(pid_t pid)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_task(pid_struct, PIDTYPE_PID);
    struct mm_struct *mm;
    struct vm_area_struct *vma;

    if (!p || !p->mm)
        return;

    mm = p->mm;
    VMA_ITERATOR(vmi, mm, 0);  // VMA_ITERATOR를 mm 초기화 후로 이동
    int vma_count = 0;
    
    for_each_vma(vmi, vma) {
        printk(KERN_INFO "[print_all_vma] pid: %d, vma start: %lx, end: %lx\n",
               pid, vma->vm_start, vma->vm_end);
        vma_count++;
    }
    printk(KERN_INFO "[print_all_vma] pid: %d, vma count: %d\n", pid, vma_count);
}

/**********************************************************
 * 올림 나눗셈 (size/obj_size가 딱 안 나누어 떨어질 수 있음)
 **********************************************************/
static inline u64 div_ceil(u64 size, u64 obj_size)
{
    return (size + obj_size - 1) / obj_size;
}

/**********************************************************
 * VMAArray 초기화
 **********************************************************/
static void init_vma_array(struct VMAArray *vma_array, size_t initial_capacity)
{
    vma_array->arr = NULL;
    vma_array->count = 0;
    vma_array->capacity = 0;

    if (initial_capacity > 0) {
        vma_array->arr = kzalloc(sizeof(struct VMA) * initial_capacity, GFP_KERNEL);
        if (vma_array->arr) {
            vma_array->capacity = initial_capacity;
        } else {
            pr_err("Failed to allocate initial VMA array\n");
        }
    }
}

/**********************************************************
 * VMAArray 해제
 **********************************************************/
void free_vma_array(struct VMAArray *vma_array)
{
    size_t i;

    // 내부 VMA들의 counts 배열 해제
    for (i = 0; i < vma_array->count; i++) {
        kfree(vma_array->arr[i].counts);
        vma_array->arr[i].counts = NULL;
    }

    kfree(vma_array->arr);
    vma_array->arr = NULL;
    vma_array->capacity = 0;
    vma_array->count = 0;
}

/**********************************************************
 * 배열 확장 (krealloc 사용)
 **********************************************************/
static int expand_vma_array(struct VMAArray *vma_array)
{
    size_t new_capacity = (vma_array->capacity == 0) ? 16 : vma_array->capacity * 2;
    size_t new_size = new_capacity * sizeof(struct VMA);
    struct VMA *new_arr;

    new_arr = krealloc(vma_array->arr, new_size, GFP_KERNEL);
    if (!new_arr) {
        pr_err("Failed to krealloc for VMA array\n");
        return -1;
    }
    vma_array->arr = new_arr;
    // 새로 늘어난 메모리 영역을 0으로 초기화해주면 좋음
    // krealloc이 내부에서 copy+free를 할 수도 있으므로, 정확한 크기만큼 처리 필요
    if (new_capacity > vma_array->capacity) {
        size_t old_size = vma_array->capacity * sizeof(struct VMA);
        memset((char *)vma_array->arr + old_size, 0, new_size - old_size);
    }

    vma_array->capacity = new_capacity;
    return 0;
}

/**********************************************************
 * start 주소 순으로 정렬하기 위한 비교 (커널 sort() 사용 시)
 **********************************************************/
static int compare_vma(const void *a, const void *b)
{
    const struct VMA *va = (const struct VMA *)a;
    const struct VMA *vb = (const struct VMA *)b;

    if (va->start < vb->start) return -1;
    if (va->start > vb->start) return 1;
    return 0;
}

/**********************************************************
 * VMA 삽입
 *   - start 기준 정렬을 유지해야 하므로, 우선 끝에 넣고
 *     나중에 sort()를 재호출하거나,
 *     혹은 직접 삽입 위치를 찾아 memmove() 해도 됨
 *   - 여기서는 간단히 "끝에 추가 후 sort()" 방식
 **********************************************************/
static int insert_chunk_vma(struct VMAArray *vma_array, u64 start, u64 size, u64 obj_size)
{
    if (vma_array->count == vma_array->capacity) {
        if (expand_vma_array(vma_array) != 0) {
            return -1;
        }
    }

    // 새 VMA 초기화
    vma_array->arr[vma_array->count].start = start;
    vma_array->arr[vma_array->count].size = size;
    vma_array->arr[vma_array->count].obj_size = obj_size;
    vma_array->arr[vma_array->count].num_objects = div_ceil(size, obj_size);
    vma_array->arr[vma_array->count].counts =
        kzalloc(sizeof(vec_info_t) * vma_array->arr[vma_array->count].num_objects, GFP_KERNEL);

    if (!vma_array->arr[vma_array->count].counts) {
        pr_err("Failed to kzalloc for counts array\n");
        return -1;
    }
    else {
        printk(KERN_INFO "[insert_chunk_vma] start: %llx, size: %llx, obj_size: %llx, num_objects: %llx\n",
               start, size, obj_size, vma_array->arr[vma_array->count].num_objects);
    }

    vma_array->count++;

    // 간단하게 전체 sort 호출
    sort(vma_array->arr, vma_array->count, sizeof(struct VMA), compare_vma, NULL);

    return 0;
}

/**********************************************************
 * VMA 삭제 (start 주소가 정확히 같은 VMA 제거)
 *   - 정확히 일치하는 VMA를 찾아서 제거
 **********************************************************/
static int remove_chunk_vma(struct VMAArray *vma_array, u64 start)
{
    size_t i;

    // 선형 검색 (정렬은 되어있지만, 정확히 일치하는 것 하나 찾으면 되므로)
    for (i = 0; i < vma_array->count; i++) {
        if (vma_array->arr[i].start == start) {
            // 해제
            kfree(vma_array->arr[i].counts);

            // 뒤쪽 VMA들을 앞으로 땡김
            if (i < vma_array->count - 1) {
                memmove(&vma_array->arr[i], 
                        &vma_array->arr[i+1],
                        (vma_array->count - (i+1)) * sizeof(struct VMA));
            }
            vma_array->count--;
            return 0;
        }
    }

    return -1; // 해당 start를 못 찾음
}

/**********************************************************
 * VMA 검색: addr가 어느 VMA에 속하는지 (이진 검색)
 **********************************************************/
static int find_chunk_vma(const struct VMAArray *vma_array, u64 addr)
{
    size_t left = 0;
    size_t right = vma_array->count;

    while (left < right) {
        size_t mid = (left + right) / 2;
        u64 vma_start = vma_array->arr[mid].start;
        u64 vma_end   = vma_start + vma_array->arr[mid].size;

        if (addr >= vma_start && addr < vma_end) {
            return (int)mid;
        }
        else if (addr < vma_start) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return -1;
}

/**********************************************************
 * 주소 접근 -> 카운트 증가
 **********************************************************/
void access_address(const struct VMAArray *vma_array, u64 addr)
{
    int idx = find_chunk_vma(vma_array, addr);
    if (idx == -1) {
        pr_info("Address 0x%llx is not in any VMA\n", addr);
        return;
    }

    {
        const struct VMA *v = &vma_array->arr[idx];
        u64 offset = addr - v->start;
        u64 obj_idx = offset / v->obj_size;

        if (obj_idx >= v->num_objects) {
            // 방어적 체크
            pr_info("Address 0x%llx out of range (should not happen)\n", addr);
            return;
        }
        // VMA의 counts 배열은 const가 아니므로 캐스팅 후 접근
        ((struct VMA *)v)->counts[obj_idx].total_accesses++;
        // printk(KERN_INFO "[access_address] addr: %llx, obj_idx: %llx, total_accesses: %d\n",
        //        addr, obj_idx, ((struct VMA *)v)->counts[obj_idx].total_accesses);
    }
}

struct VMAArray* vma_array_init(pid_t pid)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_task(pid_struct, PIDTYPE_PID);
    struct mm_struct *mm;
    struct vm_area_struct *vma;

    if (!p || !p->mm)
        return NULL;

    // Dynamically allocate instead of using a local stack variable
    struct VMAArray *vma_array_ptr = kzalloc(sizeof(struct VMAArray), GFP_KERNEL);
    if (!vma_array_ptr) {
        pr_err("Failed to allocate VMAArray\n");
        return NULL;
    }

    init_vma_array(vma_array_ptr, 4);

    mm = p->mm;
    VMA_ITERATOR(vmi, mm, 0);
    int vma_count = 0;
    
    for_each_vma(vmi, vma) {
        printk(KERN_INFO "[vma_array_init] pid: %d, vma start: %lx, end: %lx\n",
               pid, vma->vm_start, vma->vm_end);
        vma_count++;
        insert_chunk_vma(vma_array_ptr, vma->vm_start, vma->vm_end - vma->vm_start, 1536ULL);
    }
    printk(KERN_INFO "[vma_array_init] pid: %d, vma count: %d\n", pid, vma_count);

    return vma_array_ptr;
}

void vma_array_stat(struct VMAArray *vma_array)
{
    printk(KERN_INFO "[vma_array_stat] -------------------------STATISTICS------------------------\n");
    printk(KERN_INFO "[vma_array_stat] VMAArray: count=%lu, capacity=%lu\n",
           vma_array->count, vma_array->capacity);
    size_t i;
    size_t j;
    for (i = 0; i < vma_array->count; i++) {
        const struct VMA *v = &vma_array->arr[i];
        printk(KERN_INFO "[vma_array_stat] VMA %lu: start=%llx, size=%llx, obj_size=%llx, num_objects=%llx\n",
               i, v->start, v->size, v->obj_size, v->num_objects);
        for (j = 0; j < v->num_objects; j++) {
            printk(KERN_INFO "[vma_array_stat] obj_index %lu: total_accesses=%u\n",
                   j, v->counts[j].total_accesses);
        }
    }
    printk(KERN_INFO "[vma_array_stat] ----------------------------------------------------------\n");
}