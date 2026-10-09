#include "../main/app_web_helpers.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
int main() {
    CHECK(portal_text("Luz cozinha",16));
    CHECK(portal_text("Luz \"<&>",16));
    CHECK(!portal_text("12345678901234567",16));
    CHECK(!portal_text("",16));
    CHECK(portal_text("",32,true));
    CHECK(!portal_text("a\nb",16));
    CHECK(portal_text("Cozinha \xc3\xa1",16));
    CHECK(!portal_text("\xc0\x80",16));
    CHECK(!portal_text("\xed\xa0\x80",16));
    CHECK(!portal_text("\xf4\x90\x80\x80",16));
    CHECK(!portal_text("\xe2\x82",16));
    CHECK(portal_key("configurar123"));
    CHECK(!portal_key("1234567"));
    CHECK(!portal_key("senha\xc3\xa1" "abc"));
    char shortened[64] = "configurar123 senha antiga";
    memcpy(shortened, "12345678", 9); // Old bytes beyond NUL must not affect login.
    CHECK(portal_key_matches("12345678",8,shortened));
    CHECK(!portal_key_matches("1234567",7,shortened));
    CHECK(!portal_key_matches("12345679",8,shortened));
    CHECK(!portal_key_matches("",0,shortened));
    CHECK(!portal_key_matches("1234567890123456",16,"12345678"));
    const uint8_t ip[4] = {192,168,4,1};
    uint8_t query[64] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,3,'a','b','c',0,0,1,0,1};
    uint8_t packet[64];
    memcpy(packet,query,64);
    CHECK(portal_dns_reply(packet,21,64,ip)==37);
    CHECK(packet[0]==0x12 && packet[1]==0x34 && packet[7]==1);
    CHECK(memcmp(packet+33,ip,4)==0);
    memcpy(packet,query,64);packet[18]=28;
    CHECK(portal_dns_reply(packet,21,64,ip)==21 && packet[7]==0);
    memcpy(packet,query,64);
    CHECK(portal_dns_reply(packet,10,64,ip)==0);
    CHECK(portal_dns_reply(packet,20,64,ip)==0);
    CHECK(portal_dns_reply(packet,21,30,ip)==0);
    packet[12]=0xc0;
    CHECK(portal_dns_reply(packet,21,64,ip)==0);
    memcpy(packet,query,64);packet[2]=0x80;
    CHECK(portal_dns_reply(packet,21,64,ip)==0);
    memcpy(packet,query,64);packet[5]=2;
    CHECK(portal_dns_reply(packet,21,64,ip)==0);
    return 0;
}
