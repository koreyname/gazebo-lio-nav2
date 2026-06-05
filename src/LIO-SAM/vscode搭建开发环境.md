

## 开发环境搭建
在使用`vscode`开发`c++`时经常会遇到无法快捷跳转问题，因此本文就介绍一下如何在vscode中配置开发环境。

### vscode开发cpp函数快捷跳转
1. 安装cpp扩展
vscode侧边栏进入扩展选项（ctrl+shift+x)，搜索`c++`，安装`C/C++ Extension Pack`
1. 打开项目
> File->Open Folder
1. 保存为工作区
> File->Save Workspace as
选择目标目录及文件（默认以`.code-workspace`结尾）
1. 打开刚才保存的工作区文件
> File->Open File
选择刚才打开的工作区文件
1. 对工作区添加`c++`库检索路径
在刚才打开的工作区文件中，参考以下格式编辑
```yaml
{
	"folders": [
		{
			"path": "../lio-sam-optimize"
		},
	],
	"settings": {
		"C_Cpp.default.includePath": [
			"${default}",
			"/usr/include/**",
			"/usr/include/c++/9/",
			"/usr/lib/gcc/x86_64-linux-gnu/8/include/",
			"/opt/ros/humble/include/**",

			"${workspaceFolder}/include",

            // 重点！！这里的路径要设置为ros2工作空间下lio_sam_op编译出来的头文件
			"/path/to/ros2_ws/install/lio_sam_op/include/lio_sam_op"

		],
		"C_Cpp.files.exclude": {
			"**/.vscode": true,
			"**/.vs": true
		},
		"C_Cpp.default.cStandard": "c17",
		"C_Cpp.default.cppStandard": "c++17",
		"python.analysis.include": [
			"/opt/ros/humble/lib/python3.10/site-packages",
		],
		"python.autoComplete.extraPaths": [
			"/opt/ros/humble/lib/python3.10/site-packages",
		],
		"python.analysis.extraPaths": [
			"/opt/ros/humble/lib/python3.10/site-packages",
		],
	}
}
```
### 对工作区文件的解释
其实主要起作用的是工作区文件中`settings->C_Cpp.default.includePath`字段内容，vscode的cpp扩展会读取该工作空间文件下
的这个字段，并将里面的路径进行解析加到头文件解析路径中。

这里有两个路径要注意的
1. `"${workspaceFolder}/include"`，也就是Simple-LIO-SAM仓库下的include路径
2. `"/path/to/ros2_ws/install/lio_sam_op/include/lio_sam_op"`，这个要设置成你的ros工作空间对应的路径


另外，如果发现自己路径设置完成，但是有些函数还提示下划线，要看看是不是那些函数是`c++17`或者更高的标准才支持的特性，在上面的配置文件中
设置c++/c标准为17

### 其余注意
1. 尽量不要用`/path/**`的格式添加include路径，虽然看起来很省事，但会影响检索效率
1. 刚设置完路径需要等待vscode检索构建数据库，在vscode右下方状态条会提示正在进行检索
1. 上面配置文件中`"python.analysis.extraPaths","python.autoComplete.extraPaths","python.analysis.include"`是设置python语法提示器的
库检索路径，不同的python提示器插件会使用不同的路径。

