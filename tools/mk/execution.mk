# Common execution metadata gate; no board-specific assumptions.
.PHONY: check-execution
check-execution:
	$(PYTHON) tools/native/check-execution.py

.PHONY: check-flat-user
check-flat-user:
	$(PYTHON) tools/native/check-flat-user.py
