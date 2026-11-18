<?php

function nest(int $depth, string $name): void
{
    if ($depth > 0) {
        nest($depth - 1, $name);
        return;
    }
    posix_getpwnam($name);
}

while (frankenphp_handle_request(function (): void {
    if (isset($_GET['crash'])) {
        nest((int) ($_GET['depth'] ?? 0), $_GET['crash']);
    }
    echo "ok\n";
})) {
}
