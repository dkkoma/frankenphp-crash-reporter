<?php

while (frankenphp_handle_request(function (): void {
    if (isset($_GET['crash'])) {
        posix_getpwnam($_GET['crash']);
    }
    echo "ok\n";
})) {
}
