<?php

while (frankenphp_handle_request(function (): void {
    if (isset($_GET['crash'])) {
        posix_getpwnam($_GET['crash']);
    }
    if (isset($_GET['crash_in_oversized_note'])) {
        posix_getgrnam($_GET['crash_in_oversized_note']);
    }
    echo "ok\n";
})) {
}
