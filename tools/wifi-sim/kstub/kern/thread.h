typedef struct thread *thread_t; typedef int wait_result_t; typedef int kern_return_t; typedef void (*thread_continue_t)(void *, wait_result_t);
#define KERN_SUCCESS 0
extern "C" { kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *); void thread_deallocate(thread_t); }
