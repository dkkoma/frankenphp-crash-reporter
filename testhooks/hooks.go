package testhooks

/*
void test_hooks_segv(void);
int test_hooks_segv_on_c_thread(void);
int test_hooks_go_nil_on_c_thread(void);
int test_hooks_ill_on_c_thread(void);
*/
import "C"

import (
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"
)

func init() {
	addr := os.Getenv("CRASH_REPORTER_TEST_HOOKS")
	if addr == "" {
		return
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/go-nil", func(w http.ResponseWriter, _ *http.Request) {
		fmt.Fprintf(w, "recovered=%d\n", nilDeref())
	})
	mux.HandleFunc("/go-nil-on-c-thread", func(w http.ResponseWriter, _ *http.Request) {
		fmt.Fprintf(w, "recovered=%d\n", C.test_hooks_go_nil_on_c_thread())
	})
	mux.HandleFunc("/segv-in-cgo", func(http.ResponseWriter, *http.Request) {
		C.test_hooks_segv()
	})
	mux.HandleFunc("/segv-on-c-thread", func(http.ResponseWriter, *http.Request) {
		C.test_hooks_segv_on_c_thread()
	})
	mux.HandleFunc("/ill-on-c-thread", func(http.ResponseWriter, *http.Request) {
		C.test_hooks_ill_on_c_thread()
	})
	mux.HandleFunc("/notify-segv", func(http.ResponseWriter, *http.Request) {
		notified := make(chan os.Signal, 1)
		signal.Notify(notified, syscall.SIGSEGV)
		go func() {
			for range notified {
				os.Stderr.WriteString("os/signal delivered SIGSEGV\n")
			}
		}()
	})
	mux.HandleFunc("/go-panic", func(http.ResponseWriter, *http.Request) {
		go panic("test hook")
	})
	go http.ListenAndServe(addr, mux)
}

//export goNilDeref
func goNilDeref() C.int {
	return C.int(nilDeref())
}

func nilDeref() (recovered int) {
	defer func() {
		if recover() != nil {
			recovered = 1
		}
	}()
	var p *int
	return *p
}
