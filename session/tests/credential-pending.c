/* SPDX-FileCopyrightText: 2026 Vela contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* A process-identity fixture only: no wallet, D-Bus or PAM operations. */
#include <unistd.h>

int main(void)
{
    for (;;) {
        pause();
    }
}
