#!/bin/bash
# Inject CLAUDE.md contents as additionalContext on every prompt
CONTENT=$(cat /home/user/batteryLight/CLAUDE.md)
python3 -c "
import json, sys
content = sys.stdin.read()
output = {
    'hookSpecificOutput': {
        'hookEventName': 'UserPromptSubmit',
        'additionalContext': 'Project instructions (CLAUDE.md):\n' + content
    }
}
print(json.dumps(output))
" <<< "$CONTENT"
