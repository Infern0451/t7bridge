/*
 * Host-side tests for apple_proto.c (ANCS, AMS, CTS and call tracking).
 *     make -C test/host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/apple_proto.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);           \
        }                                                                  \
    } while (0)

static ancs_notification_t note(uint8_t ev, uint8_t flags, uint8_t cat, uint32_t uid)
{
    uint8_t raw[8] = {ev, flags, cat, 1, (uint8_t)uid, (uint8_t)(uid >> 8),
                      (uint8_t)(uid >> 16), (uint8_t)(uid >> 24)};
    ancs_notification_t n;
    ancs_parse_notification(raw, sizeof raw, &n);
    return n;
}

#define CALL_FLAGS (ANCS_FLAG_IMPORTANT | ANCS_FLAG_POSITIVE_ACTION | ANCS_FLAG_NEGATIVE_ACTION)

static void test_parse_and_build(void)
{
    const uint8_t raw[8] = {0x00, 0x1A, 0x01, 0x01, 0x78, 0x56, 0x34, 0x12};
    ancs_notification_t n;
    CHECK(ancs_parse_notification(raw, 8, &n), "parse NS");
    CHECK(n.event_id == 0 && n.category == 1 && n.uid == 0x12345678, "NS fields");
    CHECK((n.flags & ANCS_FLAG_POSITIVE_ACTION) && (n.flags & ANCS_FLAG_NEGATIVE_ACTION), "NS action flags");
    CHECK(!ancs_parse_notification(raw, 7, &n), "short NS rejected");

    uint8_t cmd[8];
    CHECK(ancs_build_get_title(cmd, 0x12345678, 64) == 8, "get-title length");
    const uint8_t want_get[8] = {0x00, 0x78, 0x56, 0x34, 0x12, 0x01, 0x40, 0x00};
    CHECK(memcmp(cmd, want_get, 8) == 0, "get-title bytes");
    CHECK(ancs_build_action(cmd, 0x12345678, ANCS_ACTION_POSITIVE) == 6, "action length");
    const uint8_t want_act[6] = {0x02, 0x78, 0x56, 0x34, 0x12, 0x00};
    CHECK(memcmp(cmd, want_act, 6) == 0, "answer action bytes");
}

static void test_data_source(void)
{
    /* Reply: 00 | uid | 01 | len=5 | "Alice", split across two notifications. */
    const uint8_t reply[] = {0x00, 0x78, 0x56, 0x34, 0x12, 0x01, 0x05, 0x00, 'A', 'l', 'i', 'c', 'e'};
    ancs_ds_t ds;
    ancs_ds_reset(&ds);
    uint32_t uid = 0;
    char title[64];
    CHECK(!ancs_ds_feed(&ds, reply, 6, &uid, title, sizeof title), "DS fragment 1 incomplete");
    CHECK(ancs_ds_feed(&ds, reply + 6, sizeof reply - 6, &uid, title, sizeof title), "DS complete");
    CHECK(uid == 0x12345678 && strcmp(title, "Alice") == 0, "DS title");
    /* Title longer than our buffer is truncated at a UTF-8 boundary. */
    uint8_t big[8 + 10] = {0x00, 1, 0, 0, 0, 0x01, 10, 0, 'a', 'b', 'c', 0xC3, 0xA9, 'x', 'y', 'z', 'w', 'q'};
    char small[5];
    ancs_ds_reset(&ds);
    CHECK(ancs_ds_feed(&ds, big, sizeof big, &uid, small, sizeof small), "DS long");
    CHECK(strcmp(small, "abc") == 0, "truncated without splitting UTF-8");
}

static void test_answered_then_remote_hangup(void)
{
    call_tracker_t t;
    call_tracker_reset(&t);
    uint32_t uid = 0;
    ancs_notification_t n = note(ANCS_EVENT_ADDED, CALL_FLAGS, ANCS_CAT_INCOMING_CALL, 7);
    CHECK(call_tracker_on_notification(&t, &n, 1000, &uid) == CALL_OUT_FETCH_TITLE && uid == 7, "incoming -> fetch title");
    CHECK(call_tracker_on_title(&t, 7, "Alice") == CALL_OUT_RINGING, "title -> ringing");
    CHECK(call_tracker_tick(&t, 2500) == CALL_OUT_NONE, "no duplicate ringing");
    uint32_t target;
    CHECK(call_tracker_answer_target(&t, &target) && target == 7, "TFT answer targets incoming");
    CHECK(call_tracker_reject_target(&t, &target) && target == 7, "TFT decline targets incoming");

    /* Answered: incoming removed, active call added. */
    n = note(ANCS_EVENT_REMOVED, 0, ANCS_CAT_INCOMING_CALL, 7);
    CHECK(call_tracker_on_notification(&t, &n, 3000, &uid) == CALL_OUT_NONE, "incoming removed");
    n = note(ANCS_EVENT_ADDED, ANCS_FLAG_NEGATIVE_ACTION, ANCS_CAT_ACTIVE_CALL, 8);
    CHECK(call_tracker_on_notification(&t, &n, 3100, &uid) == CALL_OUT_ACTIVE, "active call -> ACTIVE");
    CHECK(call_tracker_tick(&t, 6000) == CALL_OUT_NONE, "no end while active");
    CHECK(call_tracker_reject_target(&t, &target) && target == 8, "TFT end targets active call");

    /* Remote party hangs up: active call removed. */
    n = note(ANCS_EVENT_REMOVED, 0, ANCS_CAT_ACTIVE_CALL, 8);
    CHECK(call_tracker_on_notification(&t, &n, 7000, &uid) == CALL_OUT_NONE, "active removed");
    CHECK(call_tracker_tick(&t, 7500) == CALL_OUT_NONE, "still in grace");
    CHECK(call_tracker_tick(&t, 8600) == CALL_OUT_ENDED, "ended after grace");
    CHECK(t.phase == CALLT_IDLE, "idle after end");
    CHECK(call_tracker_tick(&t, 9000) == CALL_OUT_NONE, "ended only once");
}

static void test_declined_or_missed(void)
{
    call_tracker_t t;
    call_tracker_reset(&t);
    uint32_t uid;
    ancs_notification_t n = note(ANCS_EVENT_ADDED, CALL_FLAGS, ANCS_CAT_INCOMING_CALL, 20);
    call_tracker_on_notification(&t, &n, 0, &uid);
    call_tracker_on_title(&t, 20, "+1 555 123 4567");
    n = note(ANCS_EVENT_REMOVED, 0, ANCS_CAT_INCOMING_CALL, 20);
    call_tracker_on_notification(&t, &n, 5000, &uid);
    CHECK(call_tracker_tick(&t, 6600) == CALL_OUT_ENDED, "declined/missed ends after grace");
}

static void test_outgoing_call_ignored(void)
{
    call_tracker_t t;
    call_tracker_reset(&t);
    uint32_t uid, target;
    ancs_notification_t n = note(ANCS_EVENT_ADDED, 0, ANCS_CAT_ACTIVE_CALL, 60);
    CHECK(call_tracker_on_notification(&t, &n, 0, &uid) == CALL_OUT_NONE, "outgoing call not reported");
    CHECK(t.phase == CALLT_IDLE, "TFT stays on media during outgoing call");
    CHECK(!call_tracker_reject_target(&t, &target), "TFT End does nothing for outgoing call");
    n = note(ANCS_EVENT_REMOVED, 0, ANCS_CAT_ACTIVE_CALL, 60);
    call_tracker_on_notification(&t, &n, 5000, &uid);
    CHECK(call_tracker_tick(&t, 7000) == CALL_OUT_NONE, "no ENDED for outgoing call");
}

static void test_hold_resume(void)
{
    call_tracker_t t;
    call_tracker_reset(&t);
    uint32_t uid;
    ancs_notification_t n = note(ANCS_EVENT_ADDED, CALL_FLAGS, ANCS_CAT_INCOMING_CALL, 29);
    call_tracker_on_notification(&t, &n, 0, &uid);
    call_tracker_on_title(&t, 29, "Bob");
    n = note(ANCS_EVENT_REMOVED, 0, ANCS_CAT_INCOMING_CALL, 29);
    call_tracker_on_notification(&t, &n, 100, &uid);
    n = note(ANCS_EVENT_ADDED, ANCS_FLAG_NEGATIVE_ACTION, ANCS_CAT_ACTIVE_CALL, 30);
    CHECK(call_tracker_on_notification(&t, &n, 200, &uid) == CALL_OUT_ACTIVE, "answered -> active");
    n = note(ANCS_EVENT_REMOVED, 0, ANCS_CAT_ACTIVE_CALL, 30);
    call_tracker_on_notification(&t, &n, 1000, &uid);
    n = note(ANCS_EVENT_ADDED, ANCS_FLAG_NEGATIVE_ACTION, ANCS_CAT_ACTIVE_CALL, 31);
    CHECK(call_tracker_on_notification(&t, &n, 1500, &uid) == CALL_OUT_NONE, "resumed call is not a new ACTIVE");
    CHECK(call_tracker_tick(&t, 4000) == CALL_OUT_NONE, "no end after resume");
}

static void test_title_timeout_and_answer_before_title(void)
{
    call_tracker_t t;
    call_tracker_reset(&t);
    uint32_t uid;
    ancs_notification_t n = note(ANCS_EVENT_ADDED, CALL_FLAGS, ANCS_CAT_INCOMING_CALL, 40);
    call_tracker_on_notification(&t, &n, 0, &uid);
    CHECK(call_tracker_tick(&t, 500) == CALL_OUT_NONE, "waiting for title");
    CHECK(call_tracker_tick(&t, 1000) == CALL_OUT_RINGING, "rings without title after 1 s");
    CHECK(call_tracker_on_title(&t, 40, "Late") == CALL_OUT_NONE, "late title ignored");

    call_tracker_reset(&t);
    n = note(ANCS_EVENT_ADDED, CALL_FLAGS, ANCS_CAT_INCOMING_CALL, 41);
    call_tracker_on_notification(&t, &n, 0, &uid);
    n = note(ANCS_EVENT_ADDED, ANCS_FLAG_NEGATIVE_ACTION, ANCS_CAT_ACTIVE_CALL, 42);
    CHECK(call_tracker_on_notification(&t, &n, 200, &uid) == CALL_OUT_ACTIVE, "answered on phone fast");
    CHECK(call_tracker_tick(&t, 2000) == CALL_OUT_NONE, "no stale ringing after active");
}

static void test_other_notifications_ignored(void)
{
    call_tracker_t t;
    call_tracker_reset(&t);
    uint32_t uid;
    ancs_notification_t n = note(ANCS_EVENT_ADDED, 0, 4 /* social */, 50);
    CHECK(call_tracker_on_notification(&t, &n, 0, &uid) == CALL_OUT_NONE, "messages ignored");
    n = note(ANCS_EVENT_ADDED, 0, ANCS_CAT_MISSED_CALL, 51);
    CHECK(call_tracker_on_notification(&t, &n, 0, &uid) == CALL_OUT_NONE, "missed-call notice ignored");
    CHECK(t.phase == CALLT_IDLE, "still idle");
}

static void test_ams_cts(void)
{
    const uint8_t eu[] = {AMS_ENTITY_TRACK, AMS_TRACK_ATTR_TITLE, 0x00, 'G', 'R', 'I', 'M', 'E'};
    ams_update_t u;
    CHECK(ams_parse_entity_update(eu, sizeof eu, &u), "AMS parse");
    CHECK(u.entity == 2 && u.attribute == 2 && strcmp(u.value, "GRIME") == 0, "AMS track title");
    CHECK(ams_playback_is_playing("1,1.0,21.852"), "playing");
    CHECK(!ams_playback_is_playing("0,0.0,1.899"), "paused");
    CHECK(!ams_playback_is_playing(""), "empty = not playing");

    const uint8_t cts[10] = {0xEA, 0x07, 11, 4, 12, 38, 5, 3, 0, 0};   /* 2026-11-04 12:38:05 */
    cts_time_t t;
    CHECK(cts_parse_current_time(cts, sizeof cts, &t), "CTS parse");
    CHECK(t.year == 2026 && t.month == 11 && t.day == 4 && t.hour == 12 && t.minute == 38 && t.second == 5, "CTS fields");
    const uint8_t bad[7] = {0, 0, 0, 0, 0, 0, 0};
    CHECK(!cts_parse_current_time(bad, 7, &t), "invalid time rejected");
}

static void test_phone_number_detection(void)
{
    CHECK(looks_like_phone_number("+1 (555) 123-4567"), "formatted number");
    CHECK(!looks_like_phone_number("Alice"), "name");
    CHECK(!looks_like_phone_number("Mom 2"), "name with digit");
    CHECK(!looks_like_phone_number(""), "empty");
}

int main(void)
{
    test_parse_and_build();
    test_data_source();
    test_answered_then_remote_hangup();
    test_declined_or_missed();
    test_outgoing_call_ignored();
    test_hold_resume();
    test_title_timeout_and_answer_before_title();
    test_other_notifications_ignored();
    test_ams_cts();
    test_phone_number_detection();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
