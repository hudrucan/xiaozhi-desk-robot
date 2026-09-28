from pathlib import Path
import unittest


ROOT = Path(__file__).parents[1]


class PersistentWebSocketLifecycleTests(unittest.TestCase):
    def test_end_conversation_tts_stop_cancels_resume_and_returns_idle(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        controller = (
            ROOT / "main" / "chat" / "text_chat_controller.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'cJSON_IsTrue(cJSON_GetObjectItem(root, "end_conversation"))',
            application,
        )
        flagged_stop = application.index("if (end_conversation) {")
        normal_stop = application.index(
            "else if (!text_chat_controller_.OnTtsStop())", flagged_stop
        )
        flagged_branch = application[flagged_stop:normal_stop]
        self.assertIn("text_chat_controller_.OnTtsStop(true);", flagged_branch)
        self.assertIn("SetDeviceState(kDeviceStateIdle);", flagged_branch)

        special_case = controller.index("if (end_conversation) {")
        normal_case = controller.index("if (!pending_.load())", special_case)
        ending_branch = controller[special_case:normal_case]
        self.assertIn("pending_.store(false);", ending_branch)
        self.assertIn("resume_listening_ = false;", ending_branch)
        self.assertNotIn("ResumeListening();", ending_branch)

    def test_normal_tts_stop_keeps_existing_resume_behavior(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        normal_stop = application.index(
            "else if (!text_chat_controller_.OnTtsStop())"
        )
        sentence_start = application.index(
            'else if (strcmp(state->valuestring, "sentence_start") == 0)',
            normal_stop,
        )
        normal_branch = application[normal_stop:sentence_start]

        self.assertIn("listening_mode_ == kListeningModeManualStop", normal_branch)
        self.assertIn("SetDeviceState(kDeviceStateIdle);", normal_branch)
        self.assertIn("SetDeviceState(kDeviceStateListening);", normal_branch)


if __name__ == "__main__":
    unittest.main()
