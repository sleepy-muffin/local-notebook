# TODOs - Active

## Experience Improvements

- (Bug) The watermark is not updated if the summarization job fails. So, please update only when the job has been completed successfully.
- (Refinement) Please add a validation on clicking "Save", currently we provide one check: `response_token_budget_` is not allowed to be greater than `summary_trigger_tokens_` in Session Editor, it simply opens a Qt Message Box to explain the reason.
- (Refinement) The token count in the title bar should be honest. Currently, I discovered that it can decrease it I set the trigger to a lower value. As our design triggers the summarization once the token count have exactly surpassed the trigger amount. And the oldest message in the tail will not be truncated. So, no messages will fail to converge. Remember we never decide the messages to be included into the summarization by watermarks.
- (Refinement) Alter the send button to summarize button when the messages after the watermarks have surpassed the trigger amount, then we include all messages converged by trigger amount (the one in the tail should not be included), and it triggers summarizes only.
- (Refinement) Tasks window should not remove failed tasks, it should show the failed reasons for them. Clicking on it open a message box (content should be scrollable) which dumps the log of this task, it should not render the markdowns, and shows everything including all those are not displayed in the main window (thinking procedure, etc. )
