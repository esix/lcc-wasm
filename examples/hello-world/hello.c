/* hello.c - a "hello world" for the lcc-wasm back end.
 *
 * There is no libc here: putchar() is IMPORTED from the host (node or the
 * browser). This is the same model a winweb app uses to call its runtime --
 * the C program writes into linear memory and calls host-provided functions.
 *
 * Build:  ./build.sh            (needs ../../build/rcc and wat2wasm)
 * Run  :  node run-node.js      (node)
 *         node serve.js         then open http://localhost:8080/   (browser)
 */
int putchar(int c);              /* provided by the host, not by a C library */

int main(void) {
    char *p;
    for (p = "Hello, world from lcc-wasm!\n"; *p; p++)
        putchar(*p);
    return 0;
}
