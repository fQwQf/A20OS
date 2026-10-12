# Common execution metadata gate; no board-specific assumptions.
.PHONY: check-execution
check-execution:
	$(PYTHON) tools/check-execution.py

.PHONY: check-flat-user
check-flat-user:
	$(PYTHON) tools/check-flat-user.py
