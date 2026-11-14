#pragma once
/* Tiny DNS responder for the setup hotspot: every A query resolves to 192.168.4.1,
   so phones open the settings page as a captive portal. */
void captive_dns_start(void);
void captive_dns_stop(void);
