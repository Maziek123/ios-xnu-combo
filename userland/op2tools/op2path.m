/* op2path: what Network.framework thinks of the network (the check behind NSURLErrorNotConnectedToInternet). */
#import <Foundation/Foundation.h>
#import <Network/Network.h>
int main(void) {
	@autoreleasepool {
		nw_path_monitor_t m = nw_path_monitor_create();
		dispatch_semaphore_t sem = dispatch_semaphore_create(0);
		nw_path_monitor_set_queue(m, dispatch_get_main_queue());
		nw_path_monitor_set_update_handler(m, ^(nw_path_t p) {
			printf("path: %s\n", [[(NSObject *)p description] UTF8String]);
			nw_path_enumerate_interfaces(p, ^bool(nw_interface_t i) {
				printf("  interface %s type %d\n", nw_interface_get_name(i), (int)nw_interface_get_type(i));
				return true;
			});
			dispatch_semaphore_signal(sem);
		});
		nw_path_monitor_start(m);
		while (dispatch_semaphore_wait(sem, DISPATCH_TIME_NOW))
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, true);
		const char *hosts[] = { "1.1.1.1", "example.com" };
		for (int h = 0; h < 2; h++) {
		printf("== %s\n", hosts[h]);
		nw_endpoint_t ep = nw_endpoint_create_host(hosts[h], "80");
		nw_parameters_t pa = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
		nw_connection_t c = nw_connection_create(ep, pa);
		nw_connection_set_queue(c, dispatch_get_main_queue());
		__block int done = 0;
		nw_connection_set_state_changed_handler(c, ^(nw_connection_state_t st, nw_error_t e) {
			printf("connection state %d error %d\n", (int)st, e ? nw_error_get_error_code(e) : 0);
			nw_path_t cp = nw_connection_copy_current_path(c);
			if (cp) printf("  conn path: %s\n", [[(NSObject *)cp description] UTF8String]);
			if (st == nw_connection_state_ready || st == nw_connection_state_failed) done = 1;
		});
		nw_connection_start(c);
		for (int i = 0; i < 100 && !done; i++)
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
		nw_connection_cancel(c);
		}
	}
	return 0;
}
