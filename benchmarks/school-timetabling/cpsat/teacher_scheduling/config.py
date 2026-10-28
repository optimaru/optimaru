from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    SOLVER_TIME_LIMIT_SECONDS: float = Field(default=60.0, description="CP-SAT 탐색 제한 시간(초)")
    SOLVER_NUM_WORKERS: int = Field(default=8, description="CP-SAT 병렬 탐색 워커 수")
    SOLVER_LOG_SEARCH_PROGRESS: bool = Field(default=False, description="CP-SAT 탐색 로그 출력 여부")

    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")


env = Settings()
