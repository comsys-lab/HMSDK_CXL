extern int pebs_test_init(pid_t pid, char* cgroup_path);
extern void pebs_test_exit(void);

typedef struct { uint32_t total_accesses; } vec_info_t;
/**********************************************************
 * VMA 구조 정의
 **********************************************************/
 struct VMA {
    u64 start;         // VMA 시작 주소
    u64 size;          // VMA 크기(바이트 단위)
    u64 obj_size;      // 한 object의 크기(바이트)
    u64 num_objects;   // 이 VMA에 존재하는 object 개수
    vec_info_t *counts;       // 각 object별 access count
};

/**********************************************************
 * VMAArray: VMA들을 동적으로 관리하기 위한 구조
 **********************************************************/
struct VMAArray {
    struct VMA *arr;   // VMA 배열
    size_t capacity;   // 할당된 VMA 슬롯 수
    size_t count;      // 실제 사용중인 VMA 수
};

extern void print_all_vma(pid_t pid);
extern struct VMAArray* vma_array_init(pid_t pid);
extern void access_address(const struct VMAArray *vma_array, u64 addr);
extern void free_vma_array(struct VMAArray *vma_array);
extern void vma_array_stat(struct VMAArray *vma_array);

